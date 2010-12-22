/*****************************************************************************
MiniSat -- Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
CryptoMiniSat -- Copyright (c) 2009 Mate Soos

Original code by MiniSat authors are under an MIT licence.
Modifications for CryptoMiniSat are under GPLv3 licence.
******************************************************************************/

/**
@mainpage CryptoMiniSat
@author Mate Soos, and collaborators

CryptoMiniSat is an award-winning SAT solver based on MiniSat. It brings a
number of benefits relative to MiniSat, among them XOR clauses, extensive
failed literal probing, and better random search.

The solver basically performs the following steps:

1) parse CNF file into clause database

2) run Conflict-Driven Clause-Learning DPLL on the clauses

3) regularly run simplification passes on the clause-set

4) display solution and if not used as a library, exit

Here is a picture of of the above process in more detail:

\image html "main_flowgraph.png"

*/

#include <ctime>
#include <cstring>
#include <errno.h>
#include <string.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#ifdef _MSC_VER
#include <msvc/stdint.h>
#else
#include <stdint.h>
#endif //_MSC_VER
#include <map>
#include <set>
#include "mpi.h"

#include <signal.h>

#ifdef STATS_NEEDED
#include "Logger.h"
#endif //STATS_NEEDED

#include "time_mem.h"
#include "constants.h"
#include "DimacsParser.h"
#include "DataSyncServer.h"

#if defined(__linux__)
#include <fpu_control.h>
#endif

#include "Main.h"

Main::Main(int _argc, char** _argv) :
        numThreads(1)
        , grouping(false)
        , debugLib (false)
        , debugNewVar (false)
        , printResult (true)
        , max_nr_of_solutions (1)
        , fileNamePresent (false)
        , twoFileNamesPresent (false)
        , argc(_argc)
        , argv(_argv)
{
}

MTSolver* solversToInterrupt;
std::set<uint32_t> finished;

/**
@brief For correctly and gracefully exiting

It can happen that the user requests a dump of the learnt clauses. In this case,
the program must wait until it gets to a state where the learnt clauses are in
a correct state, then dump these and quit normally. This interrupt hander
is used to achieve this
*/
void SIGINT_handler(int signum)
{
    MTSolver& solver = *solversToInterrupt;
    printf("\n");
    std::cerr << "*** INTERRUPTED ***" << std::endl;
    if (solver.getNeedToDumpLearnts() || solver.getNeedToDumpOrig()) {
        solver.setNeedToInterrupt();
        std::cerr << "*** Please wait. We need to interrupt cleanly" << std::endl;
        std::cerr << "*** This means we might need to finish some calculations" << std::endl;
    } else {
        if (solver.getVerbosity() >= 1) solver.printStats();
        exit(1);
    }
}

void Main::readInAFile(const std::string& filename, MTSolver& solver)
{
    if (conf.verbosity >= 1) {
        std::cout << "c Reading file '" << filename << "'" << std::endl;
    }
    #ifdef DISABLE_ZLIB
        FILE * in = fopen(filename.c_str(), "rb");
    #else
        gzFile in = gzopen(filename.c_str(), "rb");
    #endif // DISABLE_ZLIB

    if (in == NULL) {
        std::cout << "ERROR! Could not open file '" << filename << "' for reading" << std::endl;
        exit(1);
    }

    DimacsParser parser(&solver, debugLib, debugNewVar, grouping);
    parser.parse_DIMACS(in);

    #ifdef DISABLE_ZLIB
        fclose(in);
    #else
        gzclose(in);
    #endif // DISABLE_ZLIB
}

void Main::readInStandardInput(MTSolver& solver)
{
    if (solver.getVerbosity()) {
        std::cout << "c Reading from standard input... Use '-h' or '--help' for help." << std::endl;
    }
    #ifdef DISABLE_ZLIB
        FILE * in = stdin;
    #else
        gzFile in = gzdopen(fileno(stdin), "rb");
    #endif // DISABLE_ZLIB

    if (in == NULL) {
        std::cout << "ERROR! Could not open standard input for reading" << std::endl;
        exit(1);
    }

    DimacsParser parser(&solver, debugLib, debugNewVar, grouping);
    parser.parse_DIMACS(in);

    #ifndef DISABLE_ZLIB
        gzclose(in);
    #endif // DISABLE_ZLIB
}



void Main::parseInAllFiles(MTSolver& solver)
{
    double myTime = cpuTime();

    //First read normal extra files
    if ((debugLib || debugNewVar) && filesToRead.size() > 0) {
        std::cout << "debugNewVar and debugLib must both be OFF to parse in extra files" << std::endl;
        exit(-1);
    }
    for (uint32_t i = 0; i < filesToRead.size(); i++) {
        readInAFile(filesToRead[i].c_str(), solver);
    }

    //Then read the main file or standard input
    if (!fileNamePresent) {
        readInStandardInput(solver);
    } else {
        readInAFile(argv[(twoFileNamesPresent ? argc-2 : argc-1)], solver);
    }

    if (conf.verbosity >= 1) {
        std::cout << "c Parsing time: "
        << std::fixed << std::setw(5) << std::setprecision(2) << (cpuTime() - myTime)
        << " s" << std::endl;
    }
}

//=================================================================================================
// Main:

void Main::printUsage(char** argv)
{
#ifdef DISABLE_ZLIB
    printf("USAGE: %s [options] <input-file> <result-output-file>\n\n  where input is plain DIMACS.\n\n", argv[0]);
#else
    printf("USAGE: %s [options] <input-file> <result-output-file>\n\n  where input may be either in plain or gzipped DIMACS.\n\n", argv[0]);
#endif // DISABLE_ZLIB
    printf("OPTIONS:\n\n");
    printf("  --polarity-mode  = {true,false,rnd,auto} [default: auto]. Selects the default\n");
    printf("                     polarity mode. Auto is the Jeroslow&Wang method\n");
    //printf("  -decay         = <num> [ 0 - 1 ]\n");
    printf("  --rnd-freq       = <num> [ 0 - 1 ]\n");
    printf("  --verbosity      = {0,1,2}\n");
    #ifdef STATS_NEEDED
    printf("  --proof-log      = Logs the proof into files 'proofN.dot', where N is the\n");
    printf("                     restart number. The log can then be visualized using\n");
    printf("                     the 'dot' program from the graphviz package\n");
    printf("  --grouping       = Lets you group clauses, and customize the groups' names.\n");
    printf("                     This helps when printing statistics\n");
    printf("  --stats          = Computes and prints statistics during the search\n");
    #endif
    printf("  --randomize      = <seed> [0 - 2^32-1] Sets random seed, used for picking\n");
    printf("                     decision variables (default = 0)\n");
    printf("  --restrict       = <num> [1 - varnum] when picking random variables to branch\n");
    printf("                     on, pick one that in the 'num' most active vars useful\n");
    printf("                     for cryptographic problems, where the question is the key,\n");
    printf("                     which is usually small (e.g. 80 bits)\n");
    printf("  --gaussuntil     = <num> Depth until which Gaussian elimination is active.\n");
    printf("                     Giving 0 switches off Gaussian elimination\n");
    printf("  --restarts       = <num> [1 - 2^32-1] No more than the given number of\n");
    printf("                     restarts will be performed during search\n");
    printf("  --nonormxorfind  = Don't find and collect >2-long xor-clauses from\n");
    printf("                     regular clauses\n");
    printf("  --nobinxorfind   = Don't find and collect 2-long xor-clauses from\n");
    printf("                     regular clauses\n");
    printf("  --noregbxorfind  = Don't regularly find and collect 2-long xor-clauses\n");
    printf("                     from regular clauses\n");
    printf("  --noextendedscc  = Don't do strongly conn. comp. finding using non-exist. bins\n");
    printf("  --noconglomerate = Don't conglomerate 2 xor clauses when one var is dependent\n");
    printf("  --nosimplify     = Don't do regular simplification rounds\n");
    printf("  --greedyunbound  = Greedily unbound variables that are not needed for SAT\n");
    printf("  --debuglib       = Solve at specific 'c Solver::solve()' points in the CNF\n");
    printf("                     file. Used to debug file generated by Solver's\n");
    printf("                     needLibraryCNFFile() function\n");
    printf("  --debugnewvar    = Add new vars at specific 'c Solver::newVar()' points in \n");
    printf("                     the CNF file. Used to debug file generated by Solver's\n");
    printf("                     needLibraryCNFFile() function.\n");
    printf("  --novarreplace   = Don't perform variable replacement. Needed for programmable\n");
    printf("                     solver feature\n");
    printf("  --restart        = {auto, static, dynamic}   Which kind of restart strategy to\n");
    printf("                     follow. Default is auto\n");
    printf("  --dumplearnts    = <filename> If interrupted or reached restart limit, dump\n");
    printf("                     the learnt clauses to the specified file. Maximum size of\n");
    printf("                     dumped clauses can be specified with next option.\n");
    printf("  --maxdumplearnts = [0 - 2^32-1] When dumping the learnts to file, what\n");
    printf("                     should be maximum length of the clause dumped. Useful\n");
    printf("                     to make the resulting file smaller. Default is 2^32-1\n");
    printf("                     note: 2-long XOR-s are always dumped.\n");
    printf("  --dumporig       = <filename> If interrupted or reached restart limit, dump\n");
    printf("                     the original problem instance, simplified to the\n");
    printf("                     current point.\n");
    printf("  --alsoread       = <filename> Also read this file in\n");
    printf("                     Can be used to re-read dumped learnts, for example\n");
    printf("  --maxsolutions   = Search for given amount of solutions\n");
    printf("                     Can only be used in single-threaded more (\"--threads=1\")\n");
    printf("  --pavgbranch     = Print average branch depth\n");
    printf("  --nofailedlit    = Don't search for failed literals, and don't search for lits\n");
    printf("                     propagated both by 'varX' and '-varX'\n");
    printf("  --noheuleprocess = Don't try to minimise XORs by XOR-ing them together.\n");
    printf("                     Algo. as per global/local substitution in Heule's thesis\n");
    printf("  --nosatelite     = Don't do clause subsumption, clause strengthening and\n");
    printf("                     variable elimination (implies -novarelim and -nosubsume1).\n");
    printf("  --noxorsubs      = Don't try to subsume xor-clauses.\n");
    printf("  --nosolprint     = Don't print the satisfying assignment if the solution\n");
    printf("                     is SAT\n");
    printf("  --novarelim      = Don't perform variable elimination as per Een and Biere\n");
    printf("  --nosubsume1     = Don't perform clause contraction through resolution\n");
    printf("  --noparthandler  = Don't find and solve subroblems with subsolvers\n");
#ifdef USE_GAUSS
    printf("  --nomatrixfind   = Don't find distinct matrixes. Put all xors into one\n");
    printf("                     big matrix\n");
    printf("  --noordercol     = Don't order variables in the columns of Gaussian\n");
    printf("                     elimination. Effectively disables iterative reduction\n");
    printf("                     of the matrix\n");
    printf("  --noiterreduce   = Don't reduce iteratively the matrix that is updated\n");
    printf("  --maxmatrixrows  = [0 - 2^32-1] Set maximum no. of rows for gaussian matrix.\n");
    printf("                     Too large matrixes should bee discarded for\n");
    printf("                     reasons of efficiency. Default: %d\n", gaussconfig.maxMatrixRows);
    printf("  --minmatrixrows  = [0 - 2^32-1] Set minimum no. of rows for gaussian matrix.\n");
    printf("                     Normally, too small matrixes are discarded for\n");
    printf("                     reasons of efficiency. Default: %d\n", gaussconfig.minMatrixRows);
    printf("  --savematrix     = [0 - 2^32-1] Save matrix every Nth decision level.\n");
    printf("                     Default: %d\n", gaussconfig.only_nth_gauss_save);
    printf("  --maxnummatrixes = [0 - 2^32-1] Maximum number of matrixes to treat.\n");
    printf("                     Default: %d\n", gaussconfig.maxNumMatrixes);
#endif //USE_GAUSS
    //printf("  --addoldlearnts  = Readd old learnts for failed variable searching.\n");
    //printf("                     These learnts are usually deleted, but may help\n");
    printf("  --nohyperbinres  = Don't add binary clauses when doing failed lit probing.\n");
    printf("  --noremovebins   = Don't remove useless binary clauses\n");
    printf("  --noremlbins     = Don't remove useless learnt binary clauses\n");
    printf("  --nosubswithbins = Don't subsume with binary clauses\n");
    printf("  --nosubswithnbins= Don't subsume with non-existent binary clauses\n");
    printf("  --noclausevivif  = Don't do perform clause vivification\n");
    printf("  --nosortwatched  = Don't sort watches according to size: bin, tri, etc.\n");
    printf("  --nolfminim      = Don't do on-the-fly self-subsuming resolution\n");
    printf("                     (called 'strong minimisation' in PrecoSat)\n");
    printf("  --norecotfssr    = Don't perform recursive/transitive OTF self-\n");
    printf("                     subsuming resolution\n");
    printf("  --nocacheotfssr  = Don't cache 1-level equeue. Less memory used, but\n");
    printf("                     disables trans OTFSSR, adv. clause vivifier, etc.\n");
    printf("  --maxgluedel     = Automatically delete clauses over max glue. See '--maxglue'\n");
    printf("  --maxglue        = [0 - 2^%d-1] default: %d. Glue value above which we\n", MAX_GLUE_BITS, conf.maxGlue);
    printf("                     throw the clause away on backtrack.\n");
    printf("  --nogates        = Don't find&replace gates\n");
    printf("  --threads        = Num threads (default is 1)");
    printf("  --syncconf       = Sync unit&bin clauses every no. conflicts\n");
    printf("\n");
}


const char* Main::hasPrefix(const char* str, const char* prefix)
{
    int len = strlen(prefix);
    if (strncmp(str, prefix, len) == 0)
        return str + len;
    else
        return NULL;
}

void Main::printResultFunc(const MTSolver& S, const lbool ret, FILE* res)
{
    if (res != NULL) {
        if (ret == l_True) {
            std::cout << "c SAT" << std::endl;
            fprintf(res, "SAT\n");
            if (printResult) {
                for (Var var = 0; var != S.nVars(); var++)
                    if (S.model[var] != l_Undef)
                        fprintf(res, "%s%d ", (S.model[var] == l_True)? "" : "-", var+1);
                    fprintf(res, "0\n");
            }
        } else if (ret == l_False) {
            std::cout << "c UNSAT" << std::endl;
            fprintf(res, "UNSAT\n");
        } else {
            std::cout << "c INCONCLUSIVE" << std::endl;
            fprintf(res, "INCONCLUSIVE\n");
        }
        fclose(res);
    } else {
        if (ret == l_True)
            std::cout << "s SATISFIABLE" << std::endl;
        else if (ret == l_False)
            std::cout << "s UNSATISFIABLE" << std::endl;

        if(ret == l_True && printResult) {
            std::stringstream toPrint;
            toPrint << "v ";
            for (Var var = 0; var != S.nVars(); var++)
                if (S.model[var] != l_Undef)
                    toPrint << ((S.model[var] == l_True)? "" : "-") << var+1 << " ";
                toPrint << "0" << std::endl;
            std::cout << toPrint.str();
        }
    }
}

void Main::parseCommandLine()
{
    const char* value;
    char tmpFilename[201];
    tmpFilename[0] = '\0';
    uint32_t unparsedOptions = 0;
    bool needTwoFileNames = false;
    conf.verbosity = 2;

    for (int i = 0; i < argc; i++) {
        if ((value = hasPrefix(argv[i], "--polarity-mode="))) {
            if (strcmp(value, "true") == 0)
                conf.polarity_mode = polarity_true;
            else if (strcmp(value, "false") == 0)
                conf.polarity_mode = polarity_false;
            else if (strcmp(value, "rnd") == 0)
                conf.polarity_mode = polarity_rnd;
            else if (strcmp(value, "auto") == 0)
                conf.polarity_mode = polarity_auto;
            else {
                printf("ERROR! unknown polarity-mode %s\n", value);
                exit(0);
            }

        } else if ((value = hasPrefix(argv[i], "--rnd-freq="))) {
            double rnd;
            if (sscanf(value, "%lf", &rnd) <= 0 || rnd < 0 || rnd > 1) {
                printf("ERROR! illegal rnRSE ERROR!d-freq constant %s\n", value);
                exit(0);
            }
            conf.random_var_freq = rnd;

        /*} else if ((value = hasPrefix(argv[i], "--decay="))) {
            double decay;
            if (sscanf(value, "%lf", &decay) <= 0 || decay <= 0 || decay > 1) {
                printf("ERROR! illegal decay constant %s\n", value);
                exit(0);
            }
            conf.var_decay = 1 / decay;*/

        } else if ((value = hasPrefix(argv[i], "--verbosity="))) {
            int verbosity = (int)strtol(value, NULL, 10);
            if (verbosity == EINVAL || verbosity == ERANGE) {
                printf("ERROR! illegal verbosity level %s\n", value);
                exit(0);
            }
            conf.verbosity = verbosity;
        #ifdef STATS_NEEDED
        } else if ((value = hasPrefix(argv[i], "--grouping"))) {
            grouping = true;
        } else if ((value = hasPrefix(argv[i], "--proof-log"))) {
            conf.needProofGraph();

        } else if ((value = hasPrefix(argv[i], "--stats"))) {
            conf.needStats();
        #endif

        } else if ((value = hasPrefix(argv[i], "--randomize="))) {
            uint32_t seed;
            if (sscanf(value, "%d", &seed) < 0) {
                printf("ERROR! illegal seed %s\n", value);
                exit(0);
            }
            conf.origSeed = seed;
        } else if ((value = hasPrefix(argv[i], "--restrict="))) {
            uint32_t branchTo;
            if (sscanf(value, "%d", &branchTo) < 0 || branchTo < 1) {
                printf("ERROR! illegal restricted pick branch number %d\n", branchTo);
                exit(0);
            }
            conf.restrictPickBranch = branchTo;
        } else if ((value = hasPrefix(argv[i], "--gaussuntil="))) {
            uint32_t until;
            if (sscanf(value, "%d", &until) < 0) {
                printf("ERROR! until %s\n", value);
                exit(0);
            }
            gaussconfig.decision_until = until;
        } else if ((value = hasPrefix(argv[i], "--restarts="))) {
            uint32_t maxrest;
            if (sscanf(value, "%d", &maxrest) < 0 || maxrest == 0) {
                printf("ERROR! illegal maximum restart number %d\n", maxrest);
                exit(0);
            }
            conf.maxRestarts = maxrest;
        } else if ((value = hasPrefix(argv[i], "--dumplearnts="))) {
            if (sscanf(value, "%200s", tmpFilename) < 0 || strlen(tmpFilename) == 0) {
                printf("ERROR! wrong filename '%s'\n", tmpFilename);
                exit(0);
            }
            conf.learntsFilename.assign(tmpFilename);
            conf.needToDumpLearnts = true;
        } else if ((value = hasPrefix(argv[i], "--dumporig="))) {
            if (sscanf(value, "%200s", tmpFilename) < 0 || strlen(tmpFilename) == 0) {
                printf("ERROR! wrong filename '%s'\n", tmpFilename);
                exit(0);
            }
            conf.origFilename.assign(tmpFilename);
            conf.needToDumpOrig = true;
        } else if ((value = hasPrefix(argv[i], "--alsoread="))) {
            if (sscanf(value, "%400s", tmpFilename) < 0 || strlen(tmpFilename) == 0) {
                printf("ERROR! wrong filename '%s'\n", tmpFilename);
                exit(0);
            }
            filesToRead.push_back(tmpFilename);
        } else if ((value = hasPrefix(argv[i], "--maxdumplearnts="))) {
            if (!conf.needToDumpLearnts) {
                printf("ERROR! -dumplearnts=<filename> must be first activated before issuing -maxdumplearnts=<size>\n");
                exit(0);
            }
            int tmp;
            if (sscanf(value, "%d", &tmp) < 0 || tmp < 0) {
                std::cout << "ERROR! wrong maximum dumped learnt clause size is illegal: " << tmp << std::endl;
                exit(0);
            }
            conf.maxDumpLearntsSize = (uint32_t)tmp;
        } else if ((value = hasPrefix(argv[i], "--maxsolutions="))) {
            int tmp;
            if (sscanf(value, "%d", &tmp) < 0 || tmp < 0) {
                std::cout << "ERROR! wrong maximum number of solutions is illegal: " << tmp << std::endl;
                exit(0);
            }
            max_nr_of_solutions = (uint32_t)tmp;

        } else if ((value = hasPrefix(argv[i], "--pavgbranch"))) {
            conf.doPrintAvgBranch = true;
        } else if ((value = hasPrefix(argv[i], "--greedyunbound"))) {
            conf.greedyUnbound = true;
        } else if ((value = hasPrefix(argv[i], "--nonormxorfind"))) {
            conf.doFindXors = false;
        } else if ((value = hasPrefix(argv[i], "--nobinxorfind"))) {
            conf.doFindEqLits = false;
        } else if ((value = hasPrefix(argv[i], "--noregbxorfind"))) {
            conf.doRegFindEqLits = false;
        } else if ((value = hasPrefix(argv[i], "--noextendedscc"))) {
            conf.doExtendedSCC = false;
        } else if ((value = hasPrefix(argv[i], "--noconglomerate"))) {
            conf.doConglXors = false;
        } else if ((value = hasPrefix(argv[i], "--nosimplify"))) {
            conf.doSchedSimp = false;
        } else if ((value = hasPrefix(argv[i], "--debuglib"))) {
            debugLib = true;
        } else if ((value = hasPrefix(argv[i], "--debugnewvar"))) {
            debugNewVar = true;
        } else if ((value = hasPrefix(argv[i], "--novarreplace"))) {
            conf.doReplace = false;
        } else if ((value = hasPrefix(argv[i], "--nofailedlit"))) {
            conf.doFailedLit = false;
        } else if ((value = hasPrefix(argv[i], "--nodisablegauss"))) {
            gaussconfig.dontDisable = true;
        } else if ((value = hasPrefix(argv[i], "--maxnummatrixes="))) {
            uint32_t maxNumMatrixes;
            if (sscanf(value, "%d", &maxNumMatrixes) < 0) {
                printf("ERROR! maxnummatrixes: %s\n", value);
                exit(0);
            }
            gaussconfig.maxNumMatrixes = maxNumMatrixes;
        } else if ((value = hasPrefix(argv[i], "--noheuleprocess"))) {
            conf.doHeuleProcess = false;
        } else if ((value = hasPrefix(argv[i], "--nosatelite"))) {
            conf.doSatELite = false;
        } else if ((value = hasPrefix(argv[i], "--noparthandler"))) {
            conf.doPartHandler = false;
        } else if ((value = hasPrefix(argv[i], "--noxorsubs"))) {
            conf.doXorSubsumption = false;
        } else if ((value = hasPrefix(argv[i], "--nohyperbinres"))) {
            conf.doHyperBinRes = false;
        } else if ((value = hasPrefix(argv[i], "--noblockedclause"))) {
            conf.doBlockedClause = false;
        } else if ((value = hasPrefix(argv[i], "--novarelim"))) {
            conf.doVarElim = false;
        } else if ((value = hasPrefix(argv[i], "--nosubsume1"))) {
            conf.doSubsume1 = false;
        } else if ((value = hasPrefix(argv[i], "--nomatrixfind"))) {
            gaussconfig.noMatrixFind = true;
        } else if ((value = hasPrefix(argv[i], "--noiterreduce"))) {
            gaussconfig.iterativeReduce = false;
        } else if ((value = hasPrefix(argv[i], "--noiterreduce"))) {
            gaussconfig.iterativeReduce = false;
        } else if ((value = hasPrefix(argv[i], "--noordercol"))) {
            gaussconfig.orderCols = false;
        } else if ((value = hasPrefix(argv[i], "--maxmatrixrows"))) {
            uint32_t rows;
            if (sscanf(value, "%d", &rows) < 0) {
                printf("ERROR! maxmatrixrows: %s\n", value);
                exit(0);
            }
            gaussconfig.maxMatrixRows = rows;
        } else if ((value = hasPrefix(argv[i], "--minmatrixrows"))) {
            uint32_t rows;
            if (sscanf(value, "%d", &rows) < 0) {
                printf("ERROR! minmatrixrows: %s\n", value);
                exit(0);
            }
            gaussconfig.minMatrixRows = rows;
        } else if ((value = hasPrefix(argv[i], "--savematrix"))) {
            uint32_t every;
            if (sscanf(value, "%d", &every) < 0) {
                printf("ERROR! savematrix: %s\n", value);
                exit(0);
            }
            std::cout << "c Matrix saved every " <<  every << " decision levels" << std::endl;
            gaussconfig.only_nth_gauss_save = every;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv);
            exit(0);
        } else if ((value = hasPrefix(argv[i], "--restart="))) {
            if (strcmp(value, "auto") == 0)
                conf.fixRestartType = auto_restart;
            else if (strcmp(value, "static") == 0)
                conf.fixRestartType = static_restart;
            else if (strcmp(value, "dynamic") == 0)
                conf.fixRestartType = dynamic_restart;
            else {
                printf("ERROR! unknown restart type %s\n", value);
                exit(0);
            }
        } else if ((value = hasPrefix(argv[i], "--nosolprint"))) {
            printResult = false;
        //} else if ((value = hasPrefix(argv[i], "--addoldlearnts"))) {
        //    conf.readdOldLearnts = true;
        } else if ((value = hasPrefix(argv[i], "--nohyperbinres"))) {
            conf.doHyperBinRes= false;
        } else if ((value = hasPrefix(argv[i], "--noremovebins"))) {
            conf.doRemUselessBins = false;
        } else if ((value = hasPrefix(argv[i], "--nosubswithnbins"))) {
            conf.doSubsWNonExistBins = false;
        } else if ((value = hasPrefix(argv[i], "--nosubswithbins"))) {
            conf.doSubsWBins = false;
        } else if ((value = hasPrefix(argv[i], "--noclausevivif"))) {
            conf.doClausVivif = false;
        } else if ((value = hasPrefix(argv[i], "--nosortwatched"))) {
            conf.doSortWatched = false;
        } else if ((value = hasPrefix(argv[i], "--nolfminim"))) {
            conf.doMinimLearntMore = false;
        } else if ((value = hasPrefix(argv[i], "--norecotfssr"))) {
            conf.doMinimLMoreRecur = false;
        } else if ((value = hasPrefix(argv[i], "--nocacheotfssr"))) {
            conf.doCacheOTFSSR = false;
        } else if ((value = hasPrefix(argv[i], "--noremlbins"))) {
            conf.doRemUselessLBins = false;
        } else if ((value = hasPrefix(argv[i], "--maxglue="))) {
            int glue = 0;
            if (sscanf(value, "%d", &glue) < 0 || glue < 2) {
                printf("ERROR! maxGlue: %s\n", value);
                exit(0);
            }
            if (glue >= (1<< MAX_GLUE_BITS)-1) {
                std::cout << "Due to memory-packing limitations, max glue cannot be more than "
                << ((1<< MAX_GLUE_BITS)-2) << std::endl;
                exit(-1);
            }
            conf.maxGlue = (uint32_t)glue;
        } else if ((value = hasPrefix(argv[i], "--maxgluedel"))) {
            conf.doMaxGlueDel = true;
        } else if ((value = hasPrefix(argv[i], "--nogates"))) {
            conf.doGateFind = false;
        } else if ((value = hasPrefix(argv[i], "--threads="))) {
            numThreads = 0;
            if (sscanf(value, "%d", &numThreads) < 0 || numThreads < 1) {
                printf("ERROR! numThreads: %s\n", value);
                exit(0);
            }
        } else if ((value = hasPrefix(argv[i], "--syncconf="))) {
            int syncEveryConf = 0;
            if (sscanf(value, "%d", &syncEveryConf) < 0 || syncEveryConf < 1) {
                printf("ERROR! syncconf: %s\n", value);
                exit(0);
            }
            conf.syncEveryConf = syncEveryConf;
        } else if (strncmp(argv[i], "-", 1) == 0 || strncmp(argv[i], "--", 2) == 0) {
            printf("ERROR! unknown flag %s\n", argv[i]);
            exit(0);
        } else {
            //std::std::cout << "argc:" << argc << " i:" << i << ", value:" << argv[i] << std::endl;
            unparsedOptions++;
            if (unparsedOptions == 2) {
                if (!(argc <= i+2)) {
                    std::cout << "You must give the input file as either:" << std::endl;
                    std::cout << " -- last option if you want the output to the console" << std::endl;
                    std::cout << " -- or one before the last option" << std::endl;
                    std::cout << "It appears that you did neither. Maybe you forgot the '--' from an option?" << std::endl;
                    exit(-1);
                }
                fileNamePresent = true;
                if (argc == i+2) needTwoFileNames = true;
            }
            if (unparsedOptions == 3) {
                if (!(argc <= i+1)) {
                    std::cout << "You must give the output file as the last option. Exiting" << std::endl;
                    exit(-1);
                }
                twoFileNamesPresent = true;
            }
            if (unparsedOptions == 4) {
                std::cout << "You gave more than two filenames as parameters." << std::endl;
                std::cout << "The first one is interpreted as the input, the second is the output." << std::endl;
                std::cout << "However, the third one I cannot do anything with. EXITING" << std::endl;
                exit(-1);
            }
        }
    }
    if (conf.verbosity >= 1) {
        if (twoFileNamesPresent) {
            std::cout << "c Outputting solution to file: " << argv[argc-1] << std::endl;
        } else {
            std::cout << "c Outputting solution to console" << std::endl;
        }
    }

    if (unparsedOptions == 2 && needTwoFileNames == true) {
        std::cout << "Command line wrong. You probably frogot to add "<< std::endl
        << "the '--'  in front of one of the options, or you started" << std::endl
        << "your output file with a hyphen ('-'). Exiting." << std::endl;
        exit(-1);
    }
    if (!debugLib) conf.libraryUsage = false;
}

FILE* Main::openOutputFile()
{
    FILE* res = NULL;
    if (twoFileNamesPresent) {
        char* filename = argv[argc-1];
        res = fopen(filename, "wb");
        if (res == NULL) {
            int backup_errno = errno;
            printf("Cannot open %s for writing. Problem: %s", filename, strerror(backup_errno));
            exit(1);
        }
    }

    return res;
}

void Main::setDoublePrecision(const uint32_t verbosity)
{
    #if defined(__linux__)
    fpu_control_t oldcw, newcw;
    _FPU_GETCW(oldcw);
    newcw = (oldcw & ~_FPU_EXTENDED) | _FPU_DOUBLE;
    _FPU_SETCW(newcw);
    if (verbosity >= 1) printf("c WARNING: for repeatability, setting FPU to use double precision\n");
#endif
}

void Main::printVersionInfo(const uint32_t verbosity)
{
    if (verbosity >= 1) printf("c This is CryptoMiniSat %s\n", VERSION);
}

const int Main::solve()
{
    int err, mpiRank, mpiSize;
    err = MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    assert(err == MPI_SUCCESS);
    err = MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
    assert(err == MPI_SUCCESS);
    if (mpiSize > 1 && mpiRank > 1) {
        conf.verbosity = 0;
        conf.origSeed = mpiRank;
        conf.simpStartMult *= 0.6;
        conf.simpStartMMult *= 0.6;
        if (mpiRank % 6 == 3) {
            conf.polarity_mode = polarity_false;
            conf.fixRestartType = dynamic_restart;
        }
        if (mpiRank % 6 == 4) {
            conf.polarity_mode = polarity_true;
            conf.fixRestartType = dynamic_restart;
        }
    }

    MTSolver solver(numThreads, conf, gaussconfig);
    solversToInterrupt = &solver;

    printVersionInfo(conf.verbosity);
    solver.printNumThreads();
    setDoublePrecision(conf.verbosity);

    parseInAllFiles(solver);
    FILE* res = openOutputFile();

    unsigned long current_nr_of_solutions = 0;
    lbool ret = l_True;
    while(current_nr_of_solutions < max_nr_of_solutions && ret == l_True) {
        ret = solver.solve();
        current_nr_of_solutions++;

        if (ret == l_True && current_nr_of_solutions < max_nr_of_solutions) {
            if (conf.verbosity >= 1) std::cout << "c Prepare for next run..." << std::endl;
            printResultFunc(solver, ret, res);

            vec<Lit> lits;
            for (Var var = 0; var != solver.nVars(); var++) {
                if (solver.model[var] != l_Undef) {
                    lits.push( Lit(var, (solver.model[var] == l_True)? true : false) );
                }
            }
            solver.addClause(lits);
        }
    }

    if (mpiSize == 1 || mpiRank == 1) {
        if (conf.needToDumpLearnts) {
            solver.dumpSortedLearnts(conf.learntsFilename, conf.maxDumpLearntsSize);
            std::cout << "c Sorted learnt clauses dumped to file '" << conf.learntsFilename << "'" << std::endl;
        }
        if (conf.needToDumpOrig) {
            solver.dumpOrigClauses(conf.origFilename);
            std::cout << "c Simplified original clauses dumped to file '" << conf.origFilename << "'" << std::endl;
        }
        if (ret == l_Undef && conf.verbosity >= 1) {
            std::cout << "c Not finished running -- maximum restart reached" << std::endl;
        }
    }
    }
    if (conf.verbosity >= 1) solver.printStats();
    printResultFunc(solver, ret, res);

    return correctReturnValue(ret);
}

int Main::correctReturnValue(const lbool ret) const
{
    int retval = -1;
    if      (ret == l_True)  retval = 10;
    else if (ret == l_False) retval = 20;
    else if (ret == l_Undef) retval = 15;
    else {
        std::cerr << "Something is very wrong, output is neither l_Undef, nor l_False, nor l_True" << std::endl;
        exit(-1);
    }

    #ifdef NDEBUG
    // (faster than "return", which will invoke the destructor for 'Solver')
    exit(retval);
    #endif
    return retval;
}

int main(int argc, char** argv)
{
    int err;
    err = MPI_Init(&argc, &argv);
    assert(err == MPI_SUCCESS);

    int mpiRank, mpiSize;
    err = MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    assert(err == MPI_SUCCESS);

    err = MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
    assert(err == MPI_SUCCESS);

    int ret;
    if (mpiSize > 1 && mpiRank == 0) {
        DataSyncServer server;
        ret = server.actAsServer();
        if (!ret) {
            FILE *f;
            f = fopen("finish", "w");
            fprintf(f, "UNSAT sync");
            fclose(f);
        }
    } else {
        Main main(argc, argv);
        main.parseCommandLine();
        signal(SIGINT, SIGINT_handler);
        //signal(SIGHUP,SIGINT_handler);
        ret = main.solve();
    }

    err = MPI_Finalize();
    assert(err == MPI_SUCCESS);

    return ret;
}

