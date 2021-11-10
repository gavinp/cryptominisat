/******************************************
Copyright (C) 2009-2020 Authors of CryptoMiniSat, see AUTHORS file

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
***********************************************/

#include "searcher.h"
#include "occsimplifier.h"
#include "time_mem.h"
#include "solver.h"
#include <iomanip>
#include "varreplacer.h"
#include "clausecleaner.h"
#include "propbyforgraph.h"
#include <algorithm>
#include <sstream>
#include <cstddef>
#include <cmath>
#include <ratio>
#include "sqlstats.h"
#include "datasync.h"
#include "reducedb.h"
#include "watchalgos.h"
#include "hasher.h"
#include "solverconf.h"
#include "distillerlong.h"
#include "xorfinder.h"
#include "vardistgen.h"
#include "solvertypes.h"
#ifdef USE_GAUSS
#include "gaussian.h"
#endif
#ifdef USE_VALGRIND
#include "valgrind/valgrind.h"
#include "valgrind/memcheck.h"
#endif

#ifdef FINAL_PREDICTOR
// #include "clustering.h"
#endif

#ifdef FINAL_PREDICTOR_BRANCH
#include "predict/maple_predictor_conf0_cluster0.h"
#endif
//#define DEBUG_RESOLV
//#define VERBOSE_DEBUG

using namespace CMSat;
using std::cout;
using std::endl;

//#define VERBOSE_DEBUG_GEN_CONFL_DOT

#ifdef VERBOSE_DEBUG
#define VERBOSE_DEBUG_GEN_CONFL_DOT
#endif

/**
@brief Sets a sane default config and allocates handler classes
*/
Searcher::Searcher(const SolverConf *_conf, Solver* _solver, std::atomic<bool>* _must_interrupt_inter) :
        HyperEngine(
            _conf
            , _solver
            , _must_interrupt_inter
        )
        , solver(_solver)
        , cla_inc(1)
{
    var_inc_vsids = 1;
    maple_step_size = conf.orig_step_size;

    more_red_minim_limit_binary_actual = conf.more_red_minim_limit_binary;
    mtrand.seed(conf.origSeed);
    hist.setSize(conf.shortTermHistorySize, conf.blocking_restart_trail_hist_length);
    cur_max_temp_red_lev2_cls = conf.max_temp_lev2_learnt_clauses;
    set_branch_strategy(0);
    polarity_mode = conf.polarity_mode;

    #ifdef FINAL_PREDICTOR
//     clustering = new ClusteringImp;
    #endif
}

Searcher::~Searcher()
{
    #ifdef USE_GAUSS
    clear_gauss_matrices();
    #endif

    #ifdef FINAL_PREDICTOR
//     delete clustering;
    #endif
}

void Searcher::new_var(
    const bool bva,
    const uint32_t orig_outer,
    bool insert_varorder)
{
    PropEngine::new_var(bva, orig_outer, insert_varorder);

    if (insert_varorder) {
        insert_var_order_all((int)nVars()-1);
        #ifdef STATS_NEEDED_BRANCH
        level_used_for_cl_arr.insert(level_used_for_cl_arr.end(), 1, 0);
        #endif
    }
}

void Searcher::new_vars(size_t n)
{
    PropEngine::new_vars(n);

    for(int i = n-1; i >= 0; i--) {
        insert_var_order_all((int)nVars()-i-1);
    }

    #ifdef STATS_NEEDED_BRANCH
    level_used_for_cl_arr.insert(level_used_for_cl_arr.end(), n, 0);
    #endif
}

void Searcher::save_on_var_memory()
{
    PropEngine::save_on_var_memory();

    #ifdef STATS_NEEDED_BRANCH
    level_used_for_cl_arr.resize(nVars());
    #endif
}

void Searcher::updateVars(
    const vector<uint32_t>& /*outerToInter*/
    , const vector<uint32_t>& interToOuter
) {

    updateArray(var_act_vsids, interToOuter);
    updateArray(var_act_maple, interToOuter);

    #ifdef VMTF_NEEDED
    rebuildOrderHeapVMTF();
    #endif
}

template<bool update_bogoprops>
inline void Searcher::add_lit_to_learnt(
    const Lit lit
    , uint32_t nDecisionLevel
) {
    const uint32_t var = lit.var();
    assert(varData[var].removed == Removed::none);

    #ifdef STATS_NEEDED_BRANCH
    if (!update_bogoprops) {
        varData[var].inside_conflict_clause_antecedents++;
        varData[var].last_seen_in_1uip = sumConflicts;
    }
    #endif

    //If var is at level 0, don't do anything with it, just skip
    if (seen[var] || varData[var].level == 0) {
        return;
    }
    seen[var] = 1;

    if (!update_bogoprops) {
        #ifdef STATS_NEEDED_BRANCH
        if (varData[var].level != 0 &&
            !level_used_for_cl_arr[varData[var].level]
        ) {
            level_used_for_cl_arr[varData[var].level] = 1;
            level_used_for_cl.push_back(varData[var].level);
        }
        #endif

        switch(branch_strategy) {
            case branch::vsids:
                vsids_bump_var_act<update_bogoprops>(var, 0.5);
                implied_by_learnts.push_back(var);
                break;

            case branch::maple:
                varData[var].maple_conflicted++;
                break;

            case branch::rand:
                break;

            #ifdef VMTF_NEEDED
            case branch::vmtf:
                implied_by_learnts.push_back(var);
                break;
            #endif
        }
    }

    if (varData[var].level >= nDecisionLevel) {
        pathC++;
    } else {
        learnt_clause.push_back(lit);
    }
}

inline void Searcher::recursiveConfClauseMin()
{
    uint32_t abstract_level = 0;
    for (size_t i = 1; i < learnt_clause.size(); i++) {
        //(maintain an abstraction of levels involved in conflict)
        abstract_level |= abstractLevel(learnt_clause[i].var());
    }

    size_t i, j;
    for (i = j = 1; i < learnt_clause.size(); i++) {
        if (varData[learnt_clause[i].var()].reason.isNULL()
            || !litRedundant(learnt_clause[i], abstract_level)
        ) {
            learnt_clause[j++] = learnt_clause[i];
        }
    }
    learnt_clause.resize(j);
}

void Searcher::normalClMinim()
{
    size_t i,j;
    for (i = j = 1; i < learnt_clause.size(); i++) {
        const PropBy& reason = varData[learnt_clause[i].var()].reason;
        size_t size;
        Lit *lits = NULL;
        PropByType type = reason.getType();
        if (type == null_clause_t) {
            learnt_clause[j++] = learnt_clause[i];
            continue;
        }

        switch (type) {
            case binary_t:
                size = 1;
                break;

            case clause_t: {
                Clause* cl2 = cl_alloc.ptr(reason.get_offset());
                lits = cl2->begin();
                size = cl2->size()-1;
                break;
            }

            #ifdef USE_GAUSS
            case xor_t: {
                vector<Lit>* xor_reason = gmatrices[reason.get_matrix_num()]->
                get_reason(reason.get_row_num());
                lits = xor_reason->data();
                size = xor_reason->size()-1;
                sumAntecedentsLits += size;
                break;
            }
            #endif

            default:
                release_assert(false);
                std::exit(-1);
        }

        for (size_t k = 0; k < size; k++) {
            Lit p;
            switch (type) {
                #ifdef USE_GAUSS
                case xor_t:
                #endif
                case clause_t:
                    p = lits[k+1];
                    break;

                case binary_t:
                    p = reason.lit2();
                    break;

                default:
                    release_assert(false);
                    std::exit(-1);
            }

            if (!seen[p.var()] && varData[p.var()].level > 0) {
                learnt_clause[j++] = learnt_clause[i];
                break;
            }
        }
    }
    learnt_clause.resize(j);
}

void Searcher::debug_print_resolving_clause(const PropBy confl) const
{
#ifndef DEBUG_RESOLV
    //Avoid unused parameter warning
    (void) confl;
#else
    switch(confl.getType()) {
        case binary_t: {
            cout << "resolv bin: " << confl.lit2() << endl;
            break;
        }

        case clause_t: {
            Clause* cl = cl_alloc.ptr(confl.get_offset());
            cout << "resolv (long): " << *cl << endl;
            break;
        }

        case xor_t: {
            //in the future, we'll have XOR clauses. Not yet.
            assert(false);
            exit(-1);
            break;
        }

        case null_clause_t: {
            assert(false);
            break;
        }
    }
#endif
}

void Searcher::update_glue_from_analysis(Clause* cl)
{
    assert(cl->red());
    if (cl->stats.is_ternary_resolvent) {
        return;
    }
    const unsigned new_glue = calc_glue(*cl);

    if (new_glue < cl->stats.glue) {
        if (cl->stats.glue <= conf.protect_cl_if_improved_glue_below_this_glue_for_one_turn) {
            cl->stats.ttl = 1;
            #if defined(STATS_NEEDED)
            red_stats_extra[cl->stats.extra_pos].ttl_stats = cl->stats.glue - new_glue;
            #endif
        }
        cl->stats.glue = new_glue;

        #ifndef FINAL_PREDICTOR
        if (cl->stats.locked_for_data_gen) {
            assert(cl->stats.which_red_array == 0);
        } else if (new_glue <= conf.glue_put_lev0_if_below_or_eq) {
            //move to lev0 if very low glue
            cl->stats.which_red_array = 0;
        } else if (new_glue <= conf.glue_put_lev1_if_below_or_eq
                && conf.glue_put_lev1_if_below_or_eq != 0
        ) {
            //move to lev1 if low glue
            cl->stats.which_red_array = 1;
        }
        #endif
     }
}

template<bool update_bogoprops>
void Searcher::add_literals_from_confl_to_learnt(
    const PropBy confl
    , const Lit p
    , uint32_t nDecisionLevel
) {
    #ifdef VERBOSE_DEBUG
    debug_print_resolving_clause(confl);
    #endif
    sumAntecedents++;

    Lit* lits = NULL;
    size_t size = 0;
    switch (confl.getType()) {
        case binary_t : {
            sumAntecedentsLits += 2;
            if (confl.isRedStep()) {
                #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
                antec_data.binRed++;
                #endif
                stats.resolvs.binRed++;
            } else {
                #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
                antec_data.binIrred++;
                #endif
                stats.resolvs.binIrred++;
            }
            break;
        }

        case clause_t : {
            Clause* cl = cl_alloc.ptr(confl.get_offset());
            assert(!cl->getRemoved());
            lits = cl->begin();
            size = cl->size();
            sumAntecedentsLits += cl->size();
            if (cl->red()) {
                stats.resolvs.longRed++;
                #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
                antec_data.longRed++;
                antec_data.glue_long_reds.push(cl->stats.glue);
                #endif
            } else {
                stats.resolvs.longIrred++;
                #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
                antec_data.longIrred++;
                #endif
            }
            #if defined(NORMAL_CL_USE_STATS)
            cl->stats.uip1_used++;
            #endif
            #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
            antec_data.size_longs.push(cl->size());
            if (!update_bogoprops) {
                cl->stats.uip1_used++;
            }
            #endif

            //If STATS_NEEDED then bump acitvity of ALL clauses
            //and set stats on all clauses
            if (!update_bogoprops
                && cl->red()
                #if !defined(STATS_NEEDED) && !defined(FINAL_PREDICTOR)
                && cl->stats.which_red_array != 0
                #endif
            ) {
                if (conf.update_glues_on_analyze) {
                    update_glue_from_analysis(cl);
                }

                #if !defined(STATS_NEEDED) && !defined(FINAL_PREDICTOR)
                if (cl->stats.which_red_array == 1)
                #endif
                    cl->stats.last_touched = sumConflicts;

                //If stats or predictor, bump all because during final
                //we will need this data and during dump when stats is on
                //we also need this data.
                #if !defined(STATS_NEEDED) && !defined(FINAL_PREDICTOR)
                if (cl->stats.which_red_array == 2)
                #endif
                    bump_cl_act<update_bogoprops>(cl);
            }
            break;
        }

        #ifdef USE_GAUSS
        case xor_t: {
            vector<Lit>* xor_reason = gmatrices[confl.get_matrix_num()]->
                get_reason(confl.get_row_num());
            lits = xor_reason->data();
            size = xor_reason->size();
            sumAntecedentsLits += size;
            break;
        }
        #endif

        case null_clause_t:
        default:
            assert(false && "Error in conflict analysis (otherwise should be UIP)");
    }

    size_t i = 0;
    bool cont = true;
    Lit x = lit_Undef;
    while(cont) {
        switch (confl.getType()) {
            case binary_t:
                if (i == 0) {
                    x = failBinLit;
                } else {
                    x = confl.lit2();
                    cont = false;
                }
                break;

            case clause_t:
            #ifdef USE_GAUSS
            case xor_t:
            #endif
                x = lits[i];
                if (i == size-1) {
                    cont = false;
                }
                break;

            case null_clause_t:
                assert(false);
                break;
        }
        if (p == lit_Undef || i > 0) {
            add_lit_to_learnt<update_bogoprops>(x, nDecisionLevel);
        }
        i++;
    }
}

template<bool update_bogoprops>
inline void Searcher::minimize_learnt_clause()
{
    const size_t origSize = learnt_clause.size();

    toClear = learnt_clause;
    if (conf.doRecursiveMinim) {
        recursiveConfClauseMin();
    } else {
        normalClMinim();
    }
    for (const Lit lit: toClear) {
        seen[lit.var()] = 0;
    }
    toClear.clear();

    stats.recMinCl += ((origSize - learnt_clause.size()) > 0);
    stats.recMinLitRem += origSize - learnt_clause.size();
}

inline void Searcher::minimize_using_bins()
{
    if (conf.doMinimRedMore
        && learnt_clause.size() > 1
    ) {
        stats.permDiff_attempt++;
        stats.moreMinimLitsStart += learnt_clause.size();
        MYFLAG++;
        const auto& ws  = watches[~learnt_clause[0]];
        uint32_t nb = 0;
        for (const Watched& w: ws) {
            if (w.isBin()) {
                Lit imp = w.lit2();
                if (permDiff[imp.var()] == MYFLAG && value(imp) == l_True) {
                    nb++;
                    permDiff[imp.var()] = MYFLAG - 1;
                }
            } else {
                break;
            }
        }
        uint32_t l = learnt_clause.size() - 1;
        if (nb > 0) {
            for (uint32_t i = 1; i < learnt_clause.size() - nb; i++) {
                if (permDiff[learnt_clause[i].var()] != MYFLAG) {
                    Lit p = learnt_clause[l];
                    learnt_clause[l] = learnt_clause[i];
                    learnt_clause[i] = p;
                    l--;
                    i--;
                }
            }
            learnt_clause.resize(learnt_clause.size()-nb);
            stats.permDiff_success++;
            stats.permDiff_rem_lits+=nb;
        }
        stats.moreMinimLitsEnd += learnt_clause.size();
    }
}

void Searcher::print_fully_minimized_learnt_clause() const
{
    if (conf.verbosity >= 6) {
        cout << "Final clause: " << learnt_clause << endl;
        for (uint32_t i = 0; i < learnt_clause.size(); i++) {
            cout << "lev learnt_clause[" << i << "]:" << varData[learnt_clause[i].var()].level << endl;
        }
    }
}

size_t Searcher::find_backtrack_level_of_learnt()
{
    if (learnt_clause.size() <= 1)
        return 0;
    else {
        uint32_t max_i = 1;
        for (uint32_t i = 2; i < learnt_clause.size(); i++) {
            if (level(learnt_clause[i]) > level(learnt_clause[max_i]))
                max_i = i;
        }
        std::swap(learnt_clause[max_i], learnt_clause[1]);
        return varData[learnt_clause[1].var()].level;
    }
}

template<bool update_bogoprops>
void Searcher::create_learnt_clause(PropBy confl)
{
    pathC = 0;
    int index = trail.size() - 1;
    Lit p = lit_Undef;

    Lit lit0 = lit_Error;
    switch (confl.getType()) {
        case binary_t : {
            lit0 = failBinLit;
            break;
        }
        #ifdef USE_GAUSS
        case xor_t: {
            vector<Lit>* cl = gmatrices[confl.get_matrix_num()]->
                get_reason(confl.get_row_num());
            lit0 = (*cl)[0];
            break;
        }
        #endif

        case clause_t : {
            lit0 = (*cl_alloc.ptr(confl.get_offset()))[0];
            break;
        }

        default:
            assert(false);
    }
    uint32_t nDecisionLevel = varData[lit0.var()].level;


    learnt_clause.push_back(lit_Undef); //make space for ~p
    do {
        #ifdef DEBUG_RESOLV
        cout << "p is: " << p << endl;
        #endif

        add_literals_from_confl_to_learnt<update_bogoprops>(
            confl, p, nDecisionLevel);

        // Select next implication to look at
        do {
            while (!seen[trail[index--].lit.var()]);
            p = trail[index+1].lit;
            assert(p != lit_Undef);
        } while(trail[index+1].lev < nDecisionLevel);

        confl = varData[p.var()].reason;
        assert(varData[p.var()].level > 0);

        //This clears out vars that haven't been added to learnt_clause,
        //but their 'seen' has been set
        seen[p.var()] = 0;

        //Okay, one more path done
        pathC--;
    } while (pathC > 0);
    assert(pathC == 0);
    learnt_clause[0] = ~p;
}

void Searcher::simple_create_learnt_clause(
    PropBy confl,
    vector<Lit>& out_learnt,
    bool True_confl
) {
    int until = -1;
    int mypathC = 0;
    Lit p = lit_Undef;
    int index = trail.size() - 1;
    assert(decisionLevel() == 1);

    do {
        if (!confl.isNULL()) {
            if (confl.getType() == binary_t) {
                if (p == lit_Undef && True_confl == false) {
                    Lit q = failBinLit;
                    if (!seen[q.var()]) {
                        seen[q.var()] = 1;
                        mypathC++;
                    }
                }
                Lit q = confl.lit2();
                if (!seen[q.var()]) {
                    seen[q.var()] = 1;
                    mypathC++;
                }
            } else {
                const Clause& c = *solver->cl_alloc.ptr(confl.get_offset());

                // if True_confl==true, then choose p begin with the 1st index of c
                for (uint32_t j = (p == lit_Undef && True_confl == false) ? 0 : 1
                    ; j < c.size()
                    ; j++
                ) {
                    Lit q = c[j];
                    assert(q.var() < seen.size());
                    if (!seen[q.var()]) {
                        seen[q.var()] = 1;
                        mypathC++;
                    }
                }
            }
        } else {
            assert(confl.isNULL());
            out_learnt.push_back(~p);
        }
        // if not break, while() will come to the index of trail blow 0, and fatal error occur;
        if (mypathC == 0) {
            break;
        }

        // Select next clause to look at:
        while (!seen[trail[index--].lit.var()]);
        // if the reason cr from the 0-level assigned var, we must break avoid move forth further;
        // but attention that maybe seen[x]=1 and never be clear. However makes no matter;
        if ((int)trail_lim[0] > index + 1
            && until == -1
        ) {
            until = out_learnt.size();
        }
        p = trail[index + 1].lit;
        confl = varData[p.var()].reason;

        //under normal circumstances this does not happen, but here, it can
        //reason is undefined for level 0
        if (varData[p.var()].level == 0) {
            confl = PropBy();
        }
        seen[p.var()] = 0;
        mypathC--;
    } while (mypathC >= 0);

    if (until != -1)
        out_learnt.resize(until);
}

void Searcher::print_debug_resolution_data(const PropBy confl)
{
#ifndef DEBUG_RESOLV
    //Avoid unused parameter warning
    (void) confl;
#else
    cout << "Before resolution, trail is: " << endl;
    print_trail();
    cout << "Conflicting clause: " << confl << endl;
    cout << "Fail bin lit: " << failBinLit << endl;
#endif
}

#ifdef VMTF_NEEDED
struct analyze_bumped_rank {
  Searcher * internal;
  analyze_bumped_rank (Searcher * i) : internal (i) { }
  uint64_t operator () (const int & a) const {
    return internal->vmtf_btab[a];
  }
};


struct analyze_bumped_smaller {
  Searcher * internal;
  analyze_bumped_smaller (Searcher * i) : internal (i) { }
  bool operator () (const int & a, const int & b) const {
    const auto s = analyze_bumped_rank (internal) (a);
    const auto t = analyze_bumped_rank (internal) (b);
    return s < t;
  }
};
#endif

template<bool update_bogoprops>
void Searcher::analyze_conflict(
    const PropBy confl
    , uint32_t& out_btlevel
    , uint32_t& glue
    , uint32_t&
    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    glue_before_minim
    #endif
) {
    //Set up environment
    #if defined(STATS_NEEDED_BRANCH) || defined(FINAL_PREDICTOR_BRANCH)
    assert(level_used_for_cl.empty());
    #ifdef SLOW_DEBUG
    for(auto& x: level_used_for_cl_arr) {
        assert(x == 0);
    }
    #endif
    #endif

    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    antec_data.clear();
    #endif

    learnt_clause.clear();
    assert(toClear.empty());
    implied_by_learnts.clear();
    assert(decisionLevel() > 0);

    print_debug_resolution_data(confl);
    create_learnt_clause<update_bogoprops>(confl);
    stats.litsRedNonMin += learnt_clause.size();
    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    glue_before_minim = calc_glue(learnt_clause);
    #endif
    minimize_learnt_clause<update_bogoprops>();
    stats.litsRedFinal += learnt_clause.size();

    //further minimisation 1 -- short, small glue clauses
    glue = std::numeric_limits<uint32_t>::max();
    if (learnt_clause.size() <= conf.max_size_more_minim) {
        glue = calc_glue(learnt_clause);
        if (glue <= conf.max_glue_more_minim) {
            minimize_using_bins();
        }
    }
    if (glue == std::numeric_limits<uint32_t>::max()) {
        glue = calc_glue(learnt_clause);
    }
    print_fully_minimized_learnt_clause();

    if (glue <= (conf.glue_put_lev0_if_below_or_eq+2)) {
        bool doit = false;
        if (conf.doMinimRedMoreMore == 1 && learnt_clause.size() <= conf.max_size_more_minim) {
            doit = true;
        }
        if (conf.doMinimRedMoreMore == 2 && learnt_clause.size() > conf.max_size_more_minim) {
            doit = true;
        }
        if (conf.doMinimRedMoreMore == 3) {
            doit = true;
        }
        if (doit) {
            minimise_redundant_more_more(learnt_clause);
        }
    }

    #ifdef STATS_NEEDED_BRANCH
    for(const Lit l: learnt_clause) {
        varData[l.var()].inside_conflict_clause++;
        varData[l.var()].inside_conflict_clause_glue += glue;
    }
    vars_used_for_cl.clear();
    for(auto& lev: level_used_for_cl) {
        vars_used_for_cl.push_back(trail[trail_lim[lev-1]].lit.var());
        assert(varData[trail[trail_lim[lev-1]].lit.var()].reason == PropBy());
        assert(level_used_for_cl_arr[lev] == 1);
        level_used_for_cl_arr[lev] = 0;
    }
    level_used_for_cl.clear();
    #endif

    out_btlevel = find_backtrack_level_of_learnt();
    if (!update_bogoprops) {
        switch(branch_strategy) {
            case branch::vsids:
                for (const uint32_t var :implied_by_learnts) {
                    if ((int32_t)varData[var].level >= (int32_t)out_btlevel-1) {
                        vsids_bump_var_act<update_bogoprops>(var, 1.0);
                    }
                }
                implied_by_learnts.clear();
                break;
            case branch::maple: {
                uint32_t bump_by = 2;
                assert(toClear.empty());
                const Lit p = learnt_clause[0];
                seen[p.var()] = true;
                toClear.push_back(p);
                for (int i = learnt_clause.size() - 1; i >= 0; i--) {
                    const uint32_t v = learnt_clause[i].var();
                    if (varData[v].reason.isClause()) {
                        ClOffset offs = varData[v].reason.get_offset();
                        Clause* cl = cl_alloc.ptr(offs);
                        for (const Lit l: *cl) {
                            if (!seen[l.var()]) {
                                seen[l.var()] = true;
                                toClear.push_back(l);
                                varData[l.var()].maple_conflicted+=bump_by;
                            }
                        }
                    } else if (varData[v].reason.getType() == binary_t) {
                        Lit l = varData[v].reason.lit2();
                        if (!seen[l.var()]) {
                            seen[l.var()] = true;
                            toClear.push_back(l);
                            varData[l.var()].maple_conflicted+=bump_by;
                        }
                        l = Lit(v, false);
                        if (!seen[l.var()]) {
                            seen[l.var()] = true;
                            toClear.push_back(l);
                            varData[l.var()].maple_conflicted+=bump_by;
                        }
                    }
                }
                for (Lit l: toClear) {
                    seen[l.var()] = 0;
                }
                toClear.clear();
                break;
            }
            #ifdef VMTF_NEEDED
            case branch::vmtf:
                std::sort(implied_by_learnts.begin(),
                          implied_by_learnts.end(),
                          analyze_bumped_smaller(this));

                for (const uint32_t var :implied_by_learnts) {
                    vmtf_bump_queue(var);
                }
                implied_by_learnts.clear();
                break;
            #endif
            default:
                break;
        }
    }
    sumConflictClauseLits += learnt_clause.size();
}

bool Searcher::litRedundant(const Lit p, uint32_t abstract_levels)
{
    #ifdef DEBUG_LITREDUNDANT
    cout << "c " << __func__ << " called" << endl;
    #endif

    analyze_stack.clear();
    analyze_stack.push(p);

    size_t top = toClear.size();
    while (!analyze_stack.empty()) {
        #ifdef DEBUG_LITREDUNDANT
        cout << "At point in litRedundant: " << analyze_stack.top() << endl;
        #endif

        const PropBy reason = varData[analyze_stack.top().var()].reason;
        PropByType type = reason.getType();
        analyze_stack.pop();

        //Must have a reason
        assert(!reason.isNULL());

        size_t size;
        Lit* lits = NULL;
        switch (type) {
            case clause_t: {
                Clause* cl = cl_alloc.ptr(reason.get_offset());
                lits = cl->begin();
                size = cl->size()-1;
                break;
            }

            #ifdef USE_GAUSS
            case xor_t: {
                vector<Lit>* xcl = gmatrices[reason.get_matrix_num()]->
                    get_reason(reason.get_row_num());
                lits = xcl->data();
                size = xcl->size()-1;
                break;
            }
            #endif

            case binary_t:
                size = 1;
                break;

            case null_clause_t:
            default:
                release_assert(false);
        }

        for (size_t i = 0
            ; i < size
            ; i++
        ) {
            Lit p2;
            switch (type) {
                #ifdef USE_GAUSS
                case xor_t:
                #endif
                case clause_t:
                    p2 = lits[i+1];
                    break;

                case binary_t:
                    p2 = reason.lit2();
                    break;

                case null_clause_t:
                default:
                    release_assert(false);
            }
            stats.recMinimCost++;

            if (!seen[p2.var()] && varData[p2.var()].level > 0) {
                if (!varData[p2.var()].reason.isNULL()
                    && (abstractLevel(p2.var()) & abstract_levels) != 0
                ) {
                    seen[p2.var()] = 1;
                    analyze_stack.push(p2);
                    toClear.push_back(p2);
                } else {
                    //Return to where we started before function executed
                    for (size_t j = top; j < toClear.size(); j++) {
                        seen[toClear[j].var()] = 0;
                    }
                    toClear.resize(top);

                    return false;
                }
            }
        }
    }

    return true;
}
template void Searcher::analyze_conflict<true>(const PropBy confl
    , uint32_t& out_btlevel
    , uint32_t& glue
    , uint32_t& glue_before_minim
);
template void Searcher::analyze_conflict<false>(const PropBy confl
    , uint32_t& out_btlevel
    , uint32_t& glue
    , uint32_t& glue_before_minim
);

bool Searcher::subset(const vector<Lit>& A, const Clause& B)
{
    //Set seen
    for (uint32_t i = 0; i != B.size(); i++)
        seen[B[i].toInt()] = 1;

    bool ret = true;
    for (uint32_t i = 0; i != A.size(); i++) {
        if (!seen[A[i].toInt()]) {
            ret = false;
            break;
        }
    }

    //Clear seen
    for (uint32_t i = 0; i != B.size(); i++)
        seen[B[i].toInt()] = 0;

    return ret;
}

void Searcher::analyze_final_confl_with_assumptions(const Lit p, vector<Lit>& out_conflict)
{
    out_conflict.clear();
    out_conflict.push_back(p);

    if (decisionLevel() == 0) {
        return;
    }

    //It's been set at level 0. The seen[] may not be large enough to do
    //seen[p.var()] -- we might have mem-saved that
    if (varData[p.var()].level == 0) {
        return;
    }

    seen[p.var()] = 1;

    assert(!trail_lim.empty());
    for (int64_t i = (int64_t)trail.size() - 1; i >= (int64_t)trail_lim[0]; i--) {
        const uint32_t x = trail[i].lit.var();
        if (seen[x]) {
            const PropBy reason = varData[x].reason;
            if (reason.isNULL()) {
                assert(varData[x].level > 0);
                out_conflict.push_back(~trail[i].lit);
            } else {
                switch(reason.getType()) {
                    case PropByType::clause_t : {
                        const Clause& cl = *cl_alloc.ptr(reason.get_offset());
                        assert(value(cl[0]) == l_True);
                        for(const Lit lit: cl) {
                            if (varData[lit.var()].level > 0) {
                                seen[lit.var()] = 1;
                            }
                        }
                        break;
                    }

                    case PropByType::binary_t: {
                        const Lit lit = reason.lit2();
                        if (varData[lit.var()].level > 0) {
                            seen[lit.var()] = 1;
                        }
                        break;
                    }

                    #ifdef USE_GAUSS
                    case PropByType::xor_t: {
                        vector<Lit>* cl = gmatrices[reason.get_matrix_num()]->
                            get_reason(reason.get_row_num());
                        assert(value((*cl)[0]) == l_True);
                        for(const Lit lit: *cl) {
                            if (varData[lit.var()].level > 0) {
                                seen[lit.var()] = 1;
                            }
                        }
                        break;
                    }
                    #endif

                    case PropByType::null_clause_t: {
                        assert(false);
                    }
                }
            }
            seen[x] = 0;
        }
    }
    seen[p.var()] = 0;

    learnt_clause = out_conflict;
    minimize_using_bins();
    out_conflict = learnt_clause;
}

void Searcher::update_assump_conflict_to_orig_outside(vector<Lit>& out_conflict)
{
    if (assumptions.empty()) {
        return;
    }

    vector<AssumptionPair> inter_assumptions;
    for(const auto& ass: assumptions) {
        inter_assumptions.push_back(
            AssumptionPair(map_outer_to_inter(ass.lit_outer), ass.lit_orig_outside));
    }

    std::sort(inter_assumptions.begin(), inter_assumptions.end());
    std::sort(out_conflict.begin(), out_conflict.end());
    assert(out_conflict.size() <= assumptions.size());
    //They now are in the order where we can go through them linearly

    /*cout << "out_conflict: " << out_conflict << endl;
    cout << "assumptions: ";
    for(AssumptionPair p: assumptions) {
        cout << "inter: " << p.lit_inter << " , outer: " << p.lit_orig_outside << " , ";
    }
    cout << endl;*/

    uint32_t at_assump = 0;
    uint32_t j = 0;
    for(size_t i = 0; i < out_conflict.size(); i++) {
        Lit lit = out_conflict[i];

        //lit_outer is actually INTER here, because we updated above
        while(lit != ~inter_assumptions[at_assump].lit_outer) {
            at_assump++;
            assert(at_assump < inter_assumptions.size() && "final conflict contains literals that are not from the assumptions!");
        }
        assert(lit == ~inter_assumptions[at_assump].lit_outer);

        //in case of symmetry breaking, we can be in trouble
        //then, the orig_outside is actually lit_Undef
        //in these cases, the symmetry breaking literal needs to be taken out
        if (inter_assumptions[at_assump].lit_orig_outside != lit_Undef) {
            //Update to correct outside lit
            out_conflict[j++] = ~inter_assumptions[at_assump].lit_orig_outside;
        }
    }
    out_conflict.resize(j);
}

void Searcher::check_blocking_restart()
{
    if (conf.do_blocking_restart
        && sumConflicts > conf.lower_bound_for_blocking_restart
        && hist.glueHist.isvalid()
        && hist.trailDepthHistLonger.isvalid()
        && decisionLevel() > 0
        && trail_lim.size() > 0
        && trail.size() > hist.trailDepthHistLonger.avg()*conf.blocking_restart_multip
    ) {
        hist.glueHist.clear();
        if (!blocked_restart) {
            stats.blocked_restart_same++;
        }
        blocked_restart = true;
        stats.blocked_restart++;
    }
}

void Searcher::print_order_heap()
{
    switch(branch_strategy) {
        case branch::vsids:
            cout << "vsids heap size: " << order_heap_vsids.size() << endl;
            cout << "vsids acts:";
            for(auto x: var_act_vsids) {
                cout << std::setprecision(12) << x.str() << " ";
            }
            cout << endl;
            cout << "VSID order heap:" << endl;
            order_heap_vsids.print_heap();
            break;
        case branch::maple:
            cout << "maple heap size: " << order_heap_maple.size() << endl;
            cout << "maple acts:";
            for(auto x: var_act_maple) {
                cout << std::setprecision(12) << x.str() << " ";
            }
            cout << endl;
            cout << "MAPLE order heap:" << endl;
            order_heap_maple.print_heap();
            break;

        case branch::rand:
            cout << "rand heap size: " << order_heap_rand.size() << endl;
            cout << "rand order heap:" << endl;
            order_heap_rand.print_heap();
            break;
        #ifdef VMTF_NEEDED
        case branch::vmtf:
            assert(false && "Not implemented yet");
            break;
        #endif
    }
}

#ifdef USE_GAUSS
void Searcher::check_need_gauss_jordan_disable()
{
    uint32_t num_disabled = 0;
    for(uint32_t i = 0; i < gqueuedata.size(); i++) {
        auto& gqd = gqueuedata[i];
        if (gqd.engaus_disable) {
            num_disabled++;
            continue;
        }

        if (conf.gaussconf.autodisable &&
            !conf.xor_detach_reattach &&
            gmatrices[i]->must_disable(gqd)
        ) {
            gqd.engaus_disable = true;
            num_disabled++;
        }

        gqd.reset();
        gmatrices[i]->update_cols_vals_set();
    }
    assert(gqhead <= qhead);

    if (num_disabled == gqueuedata.size()) {
        all_matrices_disabled = true;
        gqhead = qhead;
    }
}
#endif

lbool Searcher::search()
{
    assert(ok);
    #ifdef SLOW_DEBUG
    check_no_duplicate_lits_anywhere();
    check_order_heap_sanity();
    #endif
    const double myTime = cpuTime();

    //Stats reset & update
    stats.numRestarts++;
    hist.clear();
    hist.reset_glueHist_size(conf.shortTermHistorySize);

    assert(solver->prop_at_head());

    //Loop until restart or finish (SAT/UNSAT)
    PropBy confl;
    lbool search_ret = l_Undef;

    #ifdef VERBOSE_DEBUG
    print_order_heap();
    #endif
    while (!params.needToStopSearch
        || !confl.isNULL() //always finish the last conflict
    ) {
        #ifdef USE_GAUSS
        gqhead = qhead;
        #endif
        confl = PropBy();
        #ifdef USE_GPU
        confl = solver->datasync->pop_clauses();
        #endif
        if (!solver->okay()) {
            search_ret = l_False;
            goto end;
        }
        if (confl.isNULL()) {
            confl = propagate_any_order_fast();
        }

        if (!confl.isNULL()) {
            update_branch_params();

            #ifdef STATS_NEEDED
            stats.conflStats.update(lastConflictCausedBy);
            #endif

            print_restart_stat();
            #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
            hist.trailDepthHist.push(trail.size());
            #endif
            hist.trailDepthHistLonger.push(trail.size());
            if (!handle_conflict(confl)) {
                search_ret = l_False;
                goto end;
            }
            check_need_restart();
            #ifdef USE_GAUSS
            check_need_gauss_jordan_disable();
            #endif
        } else {
            assert(ok);
            if (decisionLevel() == 0) {
                clean_clauses_if_needed();
            }
            reduce_db_if_needed();
            lbool dec_ret;
            if (fast_backw.fast_backw_on) {
                dec_ret = new_decision_fast_backw();
            } else {
                dec_ret = new_decision<false>();
            }
            if (dec_ret != l_Undef) {
                search_ret = dec_ret;
                goto end;
            }
        }
    }
    max_confl_this_restart -= (int64_t)params.conflictsDoneThisRestart;

    cancelUntil(0);
    confl = propagate<false>();
    if (!confl.isNULL()) {
        ok = false;
        search_ret = l_False;
        goto end;
    }
    assert(solver->prop_at_head());
    if (!solver->datasync->syncData()) {
        search_ret = l_False;
        goto end;
    }
    assert(search_ret == l_Undef);

    end:
    dump_search_loop_stats(myTime);
    return search_ret;
}

inline void Searcher::update_branch_params()
{
    if ((sumConflicts & 0xfff) == 0xfff &&
        var_decay < var_decay_max)
    {
        var_decay += 0.01;
    }

    if (branch_strategy == branch::maple
        && maple_step_size > conf.min_step_size)
    {
        maple_step_size -= conf.step_size_dec;
        #ifdef VERBOSE_DEBUG
        cout << "maple step size is now: " << std::setprecision(7) << maple_step_size << endl;
        #endif
    }
}

void Searcher::dump_search_sql(const double myTime)
{
    if (solver->sqlStats) {
        solver->sqlStats->time_passed_min(
            solver
            , "search"
            , cpuTime()-myTime
        );
    }
}

/**
@brief Picks a new decision variable to branch on

@returns l_Undef if it should restart instead. l_False if it reached UNSAT
         (through simplification)
*/
template<bool update_bogoprops>
lbool Searcher::new_decision()
{
#ifdef SLOW_DEBUG
    assert(solver->prop_at_head());
#endif
    Lit next = lit_Undef;
    while (decisionLevel() < assumptions.size()) {
        // Perform user provided assumption:
        const Lit p = map_outer_to_inter(assumptions[decisionLevel()].lit_outer);
        #ifdef SLOW_DEBUG
        assert(varData[p.var()].removed == Removed::none);
        #endif

        if (value(p) == l_True) {
            // Dummy decision level:
            new_decision_level();
            #ifdef USE_GAUSS
            for(uint32_t i = 0; i < gmatrices.size(); i++) {
                assert(gmatrices[i]);
                gmatrices[i]->new_decision_level(decisionLevel());
            }
            #endif
        } else if (value(p) == l_False) {
            analyze_final_confl_with_assumptions(~p, conflict);
            return l_False;
        } else {
            assert(p.var() < nVars());
            stats.decisionsAssump++;
            next = p;
            break;
        }
    }

    if (next == lit_Undef) {
        // New variable decision:
        next = pickBranchLit();

        //No decision taken, because it's SAT
        if (next == lit_Undef)
            return l_True;

        //Update stats
        stats.decisions++;
        sumDecisions++;
    }

    // Increase decision level and enqueue 'next'
    assert(value(next) == l_Undef);
    new_decision_level();
    #ifdef USE_GAUSS
    for(uint32_t i = 0; i < gmatrices.size(); i++) {
        assert(gmatrices[i]);
        gmatrices[i]->new_decision_level(decisionLevel());
    }
    #endif
    enqueue<update_bogoprops>(next);

    return l_Undef;
}

void Searcher::update_history_stats(
    size_t backtrack_level,
    uint32_t glue,
    uint32_t connects_num_communities
) {
    assert(decisionLevel() > 0);

    //short-term averages
    hist.branchDepthHist.push(decisionLevel());
    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    hist.backtrackLevelHist.push(backtrack_level);
    hist.branchDepthHistQueue.push(decisionLevel());
    hist.numResolutionsHist.push(antec_data.num());
    #endif
    hist.branchDepthDeltaHist.push(decisionLevel() - backtrack_level);
    hist.conflSizeHist.push(learnt_clause.size());
    hist.trailDepthDeltaHist.push(trail.size() - trail_lim[backtrack_level]);

    //long-term averages
    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    hist.numResolutionsHistLT.push(antec_data.num());
    hist.decisionLevelHistLT.push(decisionLevel());
    const uint32_t overlap = antec_data.sum_size()-(antec_data.num()-1)-learnt_clause.size();
    hist.antec_data_sum_sizeHistLT.push(antec_data.sum_size());
    hist.overlapHistLT.push(overlap);
    #endif
    hist.backtrackLevelHistLT.push(backtrack_level);
    hist.conflSizeHistLT.push(learnt_clause.size());
    hist.trailDepthHistLT.push(trail.size());
    if (params.rest_type == Restart::glue) {
        hist.glueHistLTLimited.push(
            std::min<size_t>(glue, conf.max_glue_cutoff_gluehistltlimited));
    }
    hist.glueHistLT.push(glue);
    hist.glueHist.push(glue);
    hist.connects_num_communities_histLT.push(connects_num_communities);

    //Global stats from cnf.h
    sumClLBD += glue;
    sumClSize += learnt_clause.size();
}

template<bool update_bogoprops>
void Searcher::attach_and_enqueue_learnt_clause(
    Clause* cl, const uint32_t level, const bool enq)
{
    switch (learnt_clause.size()) {
        case 0:
            assert(false);
        case 1:
            //Unitary learnt
            stats.learntUnits++;
            if (enq) enqueue<false>(learnt_clause[0], level, PropBy());

            #ifdef STATS_NEEDED
            propStats.propsUnit++;
            #endif

            break;
        case 2:
            //Binary learnt
            stats.learntBins++;
            //solver->datasync->signalNewBinClause(learnt_clause);
            solver->attach_bin_clause(learnt_clause[0], learnt_clause[1], true, enq);
            if (enq) enqueue<false>(learnt_clause[0], level, PropBy(learnt_clause[1], true));

            #ifdef STATS_NEEDED
            propStats.propsBinRed++;
            #endif
            break;

        default:
            //Long learnt
            stats.learntLongs++;
            solver->attachClause(*cl, enq);
            if (enq) enqueue<false>(learnt_clause[0], level, PropBy(cl_alloc.get_offset(cl)));
            #if !defined(STATS_NEEDED) && !defined(FINAL_PREDICTOR)
            if (cl->stats.which_red_array == 2)
            #endif
                bump_cl_act<update_bogoprops>(cl);

            #ifdef STATS_NEEDED
            red_stats_extra[cl->stats.extra_pos].antec_data = antec_data;
            propStats.propsLongRed++;
            #endif

            break;
    }
}

inline void Searcher::print_learning_debug_info() const
{
    #ifndef VERBOSE_DEBUG
    return;
    #else
    cout
    << "Learning:" << learnt_clause
    << endl
    << "reverting var " << learnt_clause[0].var()+1
    << " to " << !learnt_clause[0].sign()
    << endl;
    #endif
}

void Searcher::print_learnt_clause() const
{
    if (conf.verbosity >= 6) {
        cout
        << "c learnt clause: "
        ;
        for(Lit l: learnt_clause) {
            cout << l << ": " << value(l) << " ";
        }
        cout << endl;
    }
}

#ifdef STATS_NEEDED_BRANCH
void Searcher::dump_var_for_learnt_cl(const uint32_t v,
                                      const uint64_t clid,
                                      const bool is_decision)
{
    //When it's a decision clause, the REAL clause could have already
    //set some variable to having been propagated (due to asserting clause)
    //so this assert() no longer holds for all literals
    assert(is_decision || varData[v].reason == PropBy());
    if (varData[v].dump) {
        uint64_t outer_var = map_inter_to_outer(v);
        solver->sqlStats->dec_var_clid(
            outer_var
            , varData[v].sumConflicts_at_picktime
            , clid
        );
    }
}
#endif

#ifdef STATS_NEEDED
void Searcher::dump_sql_clause_data(
    const uint32_t orig_glue
    , const uint32_t glue_before_minim
    , const uint32_t old_decision_level
    , const uint64_t clid
    , const bool is_decision
    , const uint32_t connects_num_communities
) {

    #ifdef STATS_NEEDED_BRANCH
    if (is_decision) {
        for(Lit l: learnt_clause) {
            dump_var_for_learnt_cl(l.var(), clid, is_decision);
        }
    } else {
        for(uint32_t v: vars_used_for_cl) {
            dump_var_for_learnt_cl(v, clid, is_decision);
        }
    }
    #endif

    solver->sqlStats->clause_stats(
        solver
        , clid
        , restartID
        , orig_glue
        , glue_before_minim
        , decisionLevel()
        , learnt_clause.size()
        , antec_data
        , old_decision_level
        , trail.size()
        , params.conflictsDoneThisRestart
        , restart_type_to_int(params.rest_type)
        , hist
        , is_decision
        , connects_num_communities
    );
}
#endif

#ifdef FINAL_PREDICTOR
void Searcher::set_clause_data(
    Clause* cl
    , const uint32_t orig_glue
    , const uint32_t glue_before_minim
    , const uint32_t old_decision_level
) {
    assert(cl->red());
    auto& stats_extra = red_stats_extra[cl->stats.extra_pos];

    //definitely a BUG here I think -- should be 2*antec_data.num(), no?
    //however, it's the same as how it's dumped in sqlitestats.cpp
    //stats_extra.num_overlap_literals = antec_data.sum_size()-(antec_data.num()-1)-cl->size();


    stats_extra.glueHist_longterm_avg = hist.glueHist.getLongtTerm().avg();
    stats_extra.glueHist_avg = hist.glueHist.avg_nocheck();
    stats_extra.trail_depth_level = trail.size();
    stats_extra.glue_before_minim = glue_before_minim;
    stats_extra.overlapHistLT_avg = hist.overlapHistLT.avg();
    stats_extra.num_total_lits_antecedents = antec_data.sum_size();
    stats_extra.num_antecedents = antec_data.num();
    stats_extra.numResolutionsHistLT_avg =  hist.numResolutionsHistLT.avg();
    stats_extra.conflSizeHist_avg = hist.conflSizeHist.avg();
    stats_extra.glueHistLT_avg = hist.glueHistLT.avg();
    stats_extra.antecedents_binred = antec_data.binRed;
    stats_extra.antecedents_binIrred = antec_data.binIrred;

    stats_extra.orig_glue = orig_glue;
//     stats_extra.conflSizeHistLT_avg = hist.conflSizeHistLT.avg();
//     stats_extra.branchDepthHistQueue_avg =  hist.branchDepthHistQueue.avg_nocheck();

}
#endif

#ifdef STATS_NEEDED
template<class T>
uint32_t Searcher::calc_connects_num_communities(const T& cl)
{
    assert(toClear.empty());
    uint32_t connects_num_communities = 0;
    for(const auto l: cl) {
        uint32_t comm = varData[l.var()].community_num;
        if (comm == std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        assert(comm < solver->nVars());
        if (!seen[comm]) {
            connects_num_communities++;
            toClear.push_back(Lit(comm, 0));
            seen[comm] = 1;
        }
    }

    for(auto& t: toClear) {
        seen[t.var()] = 0;
    }
    toClear.clear();

    return connects_num_communities;
}

template uint32_t Searcher::calc_connects_num_communities<Clause>(const Clause& cl);
#endif

Clause* Searcher::handle_last_confl(
    const uint32_t glue
    , const uint32_t old_decision_level
    , const uint32_t glue_before_minim
    , const bool is_decision
    , const uint32_t connects_num_communities
) {
    #ifdef STATS_NEEDED
    bool to_dump = false;
    double myrnd = mtrand.randDblExc();
    //Unfortunately, we have to change the ratio data dumped as time goes on
    //or we run out of space on CNFs that take millions(!) of conflicts
    //to solve, such as e_rphp035_05.cnf
    double decaying_ratio = (8000.0*1000.0)/((double)sumConflicts+1);
    if (decaying_ratio > 1.0) {
        decaying_ratio = 1.0;
    } else {
        //Make it more-than-linearly less
        decaying_ratio = ::pow(decaying_ratio, 1.1);
    }
    if (learnt_clause.size() > 2 && myrnd <= (conf.dump_individual_cldata_ratio*decaying_ratio)) {
        to_dump = true;
    }
    #endif

    Clause* cl;
    if (learnt_clause.size() <= 2) {
        *drat << add << learnt_clause
        #ifdef STATS_NEEDED
        << (to_dump ? clauseID : 0)
        << sumConflicts
        #endif
        << fin;
        cl = NULL;
    } else {
        cl = cl_alloc.Clause_new(learnt_clause
            , sumConflicts
            #ifdef STATS_NEEDED
            , to_dump ? clauseID : 0
            #endif
            );
        cl->isRed = true;
        cl->stats.glue = glue;
        #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
        red_stats_extra.push_back(ClauseStatsExtra());
        cl->stats.extra_pos = red_stats_extra.size()-1;
        auto& ext_stats = red_stats_extra[cl->stats.extra_pos];
        ext_stats.introduced_at_conflict = sumConflicts;
        ext_stats.orig_glue = glue;
        #endif
        cl->stats.activity = 0.0f;
        ClOffset offset = cl_alloc.get_offset(cl);
        unsigned which_arr = 2;

        #ifdef STATS_NEEDED
        ext_stats.connects_num_communities = connects_num_communities;
        ext_stats.orig_connects_num_communities = connects_num_communities;
        cl->stats.locked_for_data_gen =
            mtrand.randDblExc() < conf.lock_for_data_gen_ratio;
        #endif


        #ifndef FINAL_PREDICTOR
        if (cl->stats.locked_for_data_gen) {
            which_arr = 0;
        } else if (glue <= conf.glue_put_lev0_if_below_or_eq) {
            which_arr = 0;
        } else if (
            glue <= conf.glue_put_lev1_if_below_or_eq
            && conf.glue_put_lev1_if_below_or_eq != 0
        ) {
            which_arr = 1;
        } else {
            which_arr = 2;
        }
        #else
        which_arr = 2;
        #endif

        if (which_arr == 0) {
            stats.red_cl_in_which0++;
        }

        cl->stats.which_red_array = which_arr;
        solver->longRedCls[cl->stats.which_red_array].push_back(offset);

        *drat << add << *cl
        #ifdef STATS_NEEDED
        << sumConflicts
        #endif
        << fin;
    }

    #ifdef STATS_NEEDED
    if (solver->sqlStats
        && drat
        && conf.dump_individual_restarts_and_clauses
        && to_dump
    ) {
        assert(cl); //we only dump non-binaries to SQL
        dump_this_many_cldata_in_stream--;
        dump_sql_clause_data(
            glue
            , glue_before_minim
            , old_decision_level
            , clauseID
            , is_decision
            , connects_num_communities
        );
    }

    if (to_dump) {
        clauseID++;
    }
    #endif

    if (cl) {
        #ifdef FINAL_PREDICTOR
        set_clause_data(cl, glue, glue_before_minim, old_decision_level);
        #endif
        cl->stats.is_decision = is_decision;
    }

    return cl;
}

bool Searcher::handle_conflict(PropBy confl)
{
    stats.conflStats.numConflicts++;
    hist.num_conflicts_this_restart++;
    sumConflicts++;
    for(uint32_t i = 0; i < longRedCls.size(); i++) {
        longRedClsSizes[i] += longRedCls[i].size();
    }
    params.conflictsDoneThisRestart++;

    ConflictData data = find_conflict_level(confl);
    if (data.nHighestLevel == 0) {
        solver->ok = false;
        return false;
    }

    uint32_t backtrack_level;
    uint32_t glue;
    uint32_t glue_before_minim;
    analyze_conflict<false>(
        confl
        , backtrack_level  //return backtrack level here
        , glue             //return glue here
        , glue_before_minim         //return glue before minimization here
    );
    solver->datasync->signal_new_long_clause(learnt_clause);
    #ifdef USE_GPU
    solver->datasync->trySendAssignmentToGpu();
    #endif

    uint32_t connects_num_communities = 0;
    #ifdef STATS_NEEDED
    connects_num_communities = calc_connects_num_communities(learnt_clause);
    #endif
    print_learnt_clause();

    update_history_stats(backtrack_level, glue, connects_num_communities);
    uint32_t old_decision_level = decisionLevel();

    //Add decision-based clause in case it's short
    decision_clause.clear();
    if (conf.do_decision_based_cl
        && learnt_clause.size() > conf.decision_based_cl_min_learned_size
        && decisionLevel() <= conf.decision_based_cl_max_levels
        && decisionLevel() >= 2
    ) {
        for(int i = (int)trail_lim.size()-1; i >= 0; i--) {
            Lit l = ~trail[trail_lim[i]].lit;
            if (!seen[l.toInt()]) {
                decision_clause.push_back(l);
                seen[l.toInt()] = 1;
            }
        }
        for(Lit l: decision_clause) {
            seen[l.toInt()] = 0;
            assert(varData[l.var()].reason == PropBy());
        }
    }

    // check chrono backtrack condition
    if (conf.diff_declev_for_chrono > -1
        && (((int)decisionLevel() - (int)backtrack_level) >= conf.diff_declev_for_chrono)
    ) {
        chrono_backtrack++;
        cancelUntil(data.nHighestLevel -1);
    } else {
        non_chrono_backtrack++;
        cancelUntil(backtrack_level);
    }

    print_learning_debug_info();
    assert(value(learnt_clause[0]) == l_Undef);
    glue = std::min<uint32_t>(glue, std::numeric_limits<uint32_t>::max());
    Clause* cl = handle_last_confl(
        glue,
        old_decision_level,
        glue_before_minim,
        false, // is decision?
        connects_num_communities
    );
    attach_and_enqueue_learnt_clause<false>(cl, backtrack_level, true);

    //Add decision-based clause
    if (decision_clause.size() > 0) {
        int i = decision_clause.size();
        while(--i >= 0) {
            if (value(decision_clause[i]) == l_True
                || value(decision_clause[i]) == l_Undef
            ) {
                break;
            }
        }
        std::swap(decision_clause[0], decision_clause[i]);

        learnt_clause = decision_clause;
        print_learnt_clause();
        cl = handle_last_confl(
            learnt_clause.size(), // glue is the number of decisions, i.e. the size of decision clause
            old_decision_level,
            learnt_clause.size(), // minimized glue is the same as glue before minim
            true, // is decision?
            #ifdef STATS_NEEDED
            calc_connects_num_communities(learnt_clause)
            #else
            0
            #endif
        );
        attach_and_enqueue_learnt_clause<false>(cl, backtrack_level, false);
    }

    if (branch_strategy == branch::vsids) {
        vsids_decay_var_act();
    }
    decayClauseAct<false>();

    return true;
}

void Searcher::resetStats()
{
    startTime = cpuTime();

    //Rest solving stats
    stats.clear();
    propStats.clear();
    #ifdef STATS_NEEDED
    lastSQLPropStats = propStats;
    lastSQLGlobalStats = stats;
    #endif

    lastCleanZeroDepthAssigns = trail.size();
}

void Searcher::check_calc_satzilla_features(bool force)
{
    #ifdef STATS_NEEDED
    if (last_satzilla_feature_calc_confl == 0
        || (last_satzilla_feature_calc_confl + solver->conf.every_pred_reduce) < sumConflicts
        || force
    ) {
        last_satzilla_feature_calc_confl = sumConflicts+1;
        if (nVars() > 2
            && longIrredCls.size() > 1
            && (binTri.irredBins + binTri.redBins) > 1
        ) {
            solver->last_solve_satzilla_feature = solver->calculate_satzilla_features();
        }
    }
    #endif
}

void Searcher::check_calc_vardist_features(bool force)
{
    if (!solver->sqlStats) {
        return;
    }

    #ifdef STATS_NEEDED_BRANCH
    if (last_vardist_feature_calc_confl == 0
        || (last_vardist_feature_calc_confl + solver->conf.every_pred_reduce) < sumConflicts
        || force
    ) {
        last_vardist_feature_calc_confl = sumConflicts+1;
        VarDistGen v(solver);
        v.calc();
        latest_vardist_feature_calc++;
        v.dump();
    }
    #endif
}

void Searcher::print_restart_header()
{
    //Print restart output header
    if (((lastRestartPrintHeader == 0 && sumConflicts > 200) ||
        (lastRestartPrintHeader + 1600000) < sumConflicts)
        && conf.verbosity
    ) {
        cout
        << "c"
        << " " << std::setw(4) << "res"
        << " " << std::setw(4) << "pol"
        << " " << std::setw(4) << "bran"
        << " " << std::setw(5) << "nres"
        << " " << std::setw(5) << "conf"
        << " " << std::setw(5) << "freevar"
        << " " << std::setw(5) << "IrrL"
        << " " << std::setw(5) << "IrrB"
        << " " << std::setw(7) << "l/longC"
        << " " << std::setw(7) << "l/allC";

        for(size_t i = 0; i < longRedCls.size(); i++) {
            cout << " " << std::setw(4) << "RedL" << i;
        }

        cout
        << " " << std::setw(5) << "RedB"
        << " " << std::setw(7) << "l/longC"
        << " " << std::setw(7) << "l/allC"
        << endl;
        lastRestartPrintHeader = sumConflicts+1;
    }
}

void Searcher::print_restart_stat_line() const
{
    print_restart_stats_base();
    if (conf.print_full_restart_stat) {
        solver->print_clause_stats();
        hist.print();
    } else {
        solver->print_clause_stats();
    }

    cout << endl;
}

void Searcher::print_restart_stats_base() const
{
    cout << "c"
         << " " << std::setw(4) << restart_type_to_short_string(params.rest_type)
         << " " << std::setw(4) << polarity_mode_to_short_string(polarity_mode)
         << " " << std::setw(4) << branch_strategy_str_short
         << " " << std::setw(5) << sumRestarts();

    if (sumConflicts >  20000) {
        cout << " " << std::setw(4) << sumConflicts/1000 << "K";
    } else {
        cout << " " << std::setw(5) << sumConflicts;
    }

    cout << " " << std::setw(7) << solver->get_num_free_vars();
}

struct MyInvSorter {
    bool operator()(size_t num, size_t num2)
    {
        return num > num2;
    }
};

struct MyPolarData
{
    MyPolarData (size_t _pos, size_t _neg, size_t _flipped) :
        pos(_pos)
        , neg(_neg)
        , flipped(_flipped)
    {}

    size_t pos;
    size_t neg;
    size_t flipped;

    bool operator<(const MyPolarData& other) const
    {
        return (pos + neg) > (other.pos + other.neg);
    }
};

#ifdef STATS_NEEDED
inline void Searcher::dump_restart_sql(rst_dat_type type, int64_t clauseID)
{
    //don't dump twice for var
    if (type == rst_dat_type::var) {
        if (last_dumped_conflict_rst_data_for_var == solver->sumConflicts) {
            return;
        }
        last_dumped_conflict_rst_data_for_var = solver->sumConflicts;
    }

    //Propagation stats
    PropStats thisPropStats = propStats - lastSQLPropStats;
    SearchStats thisStats = stats - lastSQLGlobalStats;
    solver->sqlStats->restart(
        restartID
        , params.rest_type
        , thisPropStats
        , thisStats
        , solver
        , this
        , type
        , (int64_t)clauseID
    );

    if (type == rst_dat_type::norm) {
        lastSQLPropStats = propStats;
        lastSQLGlobalStats = stats;
    }
}
#endif

void Searcher::print_restart_stat()
{
    //Print restart stat
    if (conf.verbosity
        && !conf.print_all_restarts
        && ((lastRestartPrint + conf.print_restart_line_every_n_confl)
          < sumConflicts)
    ) {
        print_restart_stat_line();
        lastRestartPrint = sumConflicts;
    }
}

void Searcher::reset_temp_cl_num()
{
    cur_max_temp_red_lev2_cls = conf.max_temp_lev2_learnt_clauses;
}

void Searcher::reduce_db_if_needed()
{
    #ifdef NORMAL_CL_USE_STATS
    if (conf.every_pred_reduce != 0
        && sumConflicts >= next_pred_reduce
    ) {
        solver->reduceDB->gather_normal_cl_use_stats();
        next_pred_reduce = sumConflicts + conf.every_pred_reduce;
    }
    #endif

    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    if (conf.every_pred_reduce != 0
        && sumConflicts >= next_pred_reduce
    ) {
        #ifdef STATS_NEEDED
        if (solver->sqlStats) {
            solver->reduceDB->dump_sql_cl_data(restart_type_to_int(params.rest_type));
        }
        #endif
        #ifdef FINAL_PREDICTOR
        solver->reduceDB->handle_predictors();
        cl_alloc.consolidate(solver);
        #endif
        next_pred_reduce = sumConflicts + conf.every_pred_reduce;
    }
    #endif

    #ifndef FINAL_PREDICTOR
    if (conf.every_lev1_reduce != 0
        && sumConflicts >= next_lev1_reduce
    ) {
        solver->reduceDB->handle_lev1();
        next_lev1_reduce = sumConflicts + conf.every_lev1_reduce;
    }

    if (conf.every_lev2_reduce != 0) {
        if (sumConflicts >= next_lev2_reduce) {
            solver->reduceDB->handle_lev2();
            cl_alloc.consolidate(solver);
            next_lev2_reduce = sumConflicts + conf.every_lev2_reduce;
        }
    } else {
        if (longRedCls[2].size() > cur_max_temp_red_lev2_cls) {
            solver->reduceDB->handle_lev2();
            cur_max_temp_red_lev2_cls *= conf.inc_max_temp_lev2_red_cls;
            cl_alloc.consolidate(solver);
        }
    }
    #endif
}

void Searcher::clean_clauses_if_needed()
{
    #ifdef SLOW_DEBUG
    assert(decisionLevel() == 0);
    assert(qhead == trail.size());
    #endif

    const size_t newZeroDepthAss = trail.size() - lastCleanZeroDepthAssigns;
    if (newZeroDepthAss > 0
        && simpDB_props < 0
        && newZeroDepthAss > ((double)nVars()*0.05)
    ) {
        if (conf.verbosity >= 2) {
            cout << "c newZeroDepthAss : " << newZeroDepthAss
            << " -- "
            << (double)newZeroDepthAss/(double)nVars()*100.0
            << " % of active vars"
            << endl;
        }
        lastCleanZeroDepthAssigns = trail.size();
        solver->clauseCleaner->remove_and_clean_all();

        cl_alloc.consolidate(solver);
        //TODO this is not needed, but seems to help speed
        //     perhaps because it re-shuffles
        rebuildOrderHeap();

        simpDB_props = (litStats.redLits + litStats.irredLits)<<5;
    }
}

void Searcher::rebuildOrderHeap()
{
    if (conf.verbosity) {
        cout << "c [branch] rebuilding order heap for all branchings. Current branching: " <<
        branch_type_to_string(branch_strategy) << endl;
    }
    vector<uint32_t> vs;
    vs.reserve(nVars());
    for (uint32_t v = 0; v < nVars(); v++) {
        if (varData[v].removed != Removed::none
            //NOTE: the level==0 check is needed because SLS calls this
            //when there is a solution already, but we should only skip
            //level 0 assignments
            || (value(v) != l_Undef && varData[v].level == 0)
        ) {
            continue;
        } else {
            vs.push_back(v);
        }
    }

    #ifdef VERBOSE_DEBUG
    cout << "c [branch] Building VSDIS order heap" << endl;
    #endif
    order_heap_vsids.build(vs);

    #ifdef VERBOSE_DEBUG
    cout << "c [branch] Building MAPLE order heap" << endl;
    #endif
    order_heap_maple.build(vs);

    #ifdef VERBOSE_DEBUG
    cout << "c [branch] Building RAND order heap" << endl;
    #endif
    order_heap_rand.build(vs);

    #ifdef VMTF_NEEDED
    rebuildOrderHeapVMTF();
    #endif
}

#ifdef VMTF_NEEDED
void Searcher::rebuildOrderHeapVMTF()
{
    #ifdef VERBOSE_DEBUG
    cout << "c [branch] Building VMTF order heap" << endl;
    #endif
    //TODO fix
    return;

    vector<uint32_t> vs;
    vs.reserve(nVars());
    uint32_t v = pick_var_vmtf();
    while(v != var_Undef) {
        if (varData[v].removed != Removed::none
            //NOTE: the level==0 check is needed because SLS calls this
            //when there is a solution already, but we should only skip
            //level 0 assignments
            || (value(v) != l_Undef && varData[v].level == 0)
        ) {
            //
        } else {
            vs.push_back(v);
        }
        v = pick_var_vmtf();
        cout << "v: " << v << endl;
    }

    //Clear it out
    vmtf_queue = Queue();
    vmtf_btab.clear();
    vmtf_links.clear();
    vmtf_btab.insert(vmtf_btab.end(), nVars(), 0);
    vmtf_links.insert(vmtf_links.end(), nVars(), Link());

    //Insert in reverse order
    for(int i = (int)vs.size()-1; i >= 0; i--) {
        vmtf_init_enqueue(vs[v]);
    }
}
#endif

struct branch_type_total{
    branch_type_total() {}
    branch_type_total (CMSat::branch _branch,
                       double _decay_start, double _decay_max,
                       string _descr, string _descr_short) :
        branch(_branch),
        decay_start(_decay_start),
        decay_max(_decay_max),
        descr(_descr),
        descr_short(_descr_short)
    {}
    explicit branch_type_total(CMSat::branch _branch) :
        branch(_branch)
    {}

    CMSat::branch branch = CMSat::branch::vsids;
    double decay_start = 0.95;
    double decay_max = 0.95;
    string descr;
    string descr_short;
};

void Searcher::set_branch_strategy(uint32_t iteration_num)
{
    size_t smallest = 0;
    size_t start = 0;
    vector<branch_type_total> select;
    if (conf.verbosity) {
        if (conf.verbosity >= 2) {
            cout << "c [branch] orig text: " << conf.branch_strategy_setup << endl;
        }
        cout << "c [branch] selection: ";
    }

    while(smallest !=std::string::npos) {
        smallest = std::string::npos;

        size_t vsidsx_once = conf.branch_strategy_setup.find("vsidsx_once", start);
        smallest = std::min(vsidsx_once, smallest);

        size_t vsidsx = conf.branch_strategy_setup.find("vsidsx", start);
        smallest = std::min(vsidsx, smallest);

        size_t vsids1 = conf.branch_strategy_setup.find("vsids1", start);
        smallest = std::min(vsids1, smallest);

        size_t vsids2 = conf.branch_strategy_setup.find("vsids2", start);
        smallest = std::min(vsids2, smallest);

        #ifdef VMTF_NEEDED
        size_t vmtf = conf.branch_strategy_setup.find("vmtf", start);
        smallest = std::min(vmtf, smallest);
        #endif

        size_t maple1 = conf.branch_strategy_setup.find("maple1", start);
        smallest = std::min(maple1, smallest);

        size_t maple2 = conf.branch_strategy_setup.find("maple2", start);
        smallest = std::min(maple2, smallest);

        size_t rand = conf.branch_strategy_setup.find("rand", start);
        smallest = std::min(rand, smallest);

        if (smallest == std::string::npos) {
            break;
        }

        if (conf.verbosity && !select.empty()) {
            cout << "+";
        }

        if (smallest == vsidsx_once) {
            select.push_back(branch_type_total(branch::vsids, 0.80, 0.95, "VSIDSXONCE", "vxo"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        } else if (smallest == vsidsx) {
            select.push_back(branch_type_total(branch::vsids, 0.80, 0.95, "VSIDSX", "vx"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        }
        else if (smallest == vsids1) {
            select.push_back(branch_type_total(branch::vsids, 0.92, 0.92, "VSIDS1", "vs1"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        }
        else if (smallest == vsids2) {
            select.push_back(branch_type_total(branch::vsids, 0.99, 0.99, "VSIDS2", "vs2"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        }
        #ifdef VMTF_NEEDED
        else if (smallest == vmtf) {
            select.push_back(branch_type_total(branch::vmtf, 0, 0, "VMTF", "vmt"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        }
        #endif
        else if (smallest == maple1) {
            //TODO should we do this incremental stuff?
            //maple_step_size = conf.orig_step_size;
            select.push_back(branch_type_total(branch::maple, 0.70, 0.70, "MAPLE1", "mp1"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        }
        else if (smallest == maple2) {
            //TODO should we do this incremental stuff?
            //maple_step_size = conf.orig_step_size;
            select.push_back(branch_type_total(branch::maple, 0.90, 0.90, "MAPLE2", "mp2"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        }
        else if (smallest == rand) {
            select.push_back(branch_type_total(branch::rand, 1, 1, "RAND", "rand"));
            if (conf.verbosity) {
                cout << select[select.size()-1].descr;
            }
        } else {
            assert(false);
        }

        //Search for next one. The strings are quite distinct, this works.
        start = smallest + 3;
    }
    if (conf.verbosity) {
        cout << " -- total: " << select.size() << endl;
    }

    assert(!select.empty());

    //Deal with systems that are only meant to be done once
    if (iteration_num >= select.size()) {
        uint32_t j = 0;
        for(uint32_t i = 0; i < select.size(); i ++) {
            if (select[i].descr != "VSIDSXONCE") {
                select[j++] = select[i];
            } else {
                iteration_num--;
            }
        }
        select.resize(j);
    }

    uint32_t which = iteration_num % select.size();
    branch_strategy = select[which].branch;
    branch_strategy_str = select[which].descr;
    branch_strategy_str_short = select[which].descr_short;
    var_decay = select[which].decay_start;
    var_decay_max = select[which].decay_max;

    if (branch_strategy == branch::maple) {
        cur_rest_type = Restart::luby;
    } else {
        cur_rest_type = conf.restartType;
    }

    if (conf.verbosity) {
        cout << "c [branch] adjusting to: "
        << branch_type_to_string(branch_strategy)
        << " var_decay_max:" << var_decay << " var_decay:" << var_decay
        << " descr: " << select[which].descr
        << endl;
    }
}

inline void Searcher::dump_search_loop_stats(double myTime)
{
    #if defined(STATS_NEEDED) || defined(FINAL_PREDICTOR)
    check_calc_satzilla_features();
    check_calc_vardist_features();
    #endif

    print_restart_header();
    dump_search_sql(myTime);
    if (conf.verbosity && conf.print_all_restarts) {
        print_restart_stat_line();
    }
    #ifdef STATS_NEEDED
    if (sqlStats
        && conf.dump_individual_restarts_and_clauses
    ) {
        dump_restart_sql(rst_dat_type::norm);
    }
    #endif
    restartID++;
}

bool Searcher::must_abort(const lbool status) {
    if (status != l_Undef) {
        if (conf.verbosity >= 6) {
            cout
            << "c Returned status of search() is " << status << " at confl:"
            << sumConflicts
            << endl;
        }
        return true;
    }

    if (stats.conflStats.numConflicts >= max_confl_per_search_solve_call) {
        if (conf.verbosity >= 3) {
            cout
            << "c search over max conflicts"
            << endl;
        }
        return true;
    }

    if (cpuTime() >= conf.maxTime) {
        if (conf.verbosity >= 3) {
            cout
            << "c search over max time"
            << endl;
        }
        return true;
    }

    if (solver->must_interrupt_asap()) {
        if (conf.verbosity >= 3) {
            cout
            << "c search interrupting as requested"
            << endl;
        }
        return true;
    }

    return false;
}

void Searcher::setup_polarity_strategy()
{
    //Set to default first
    polarity_mode = conf.polarity_mode;
    polar_stable_longest_trail_this_iter = 0;

    if (polarity_mode == PolarityMode::polarmode_automatic) {
        if (branch_strategy_num > 0 &&
            conf.polar_stable_every_n > 0 &&
            ((branch_strategy_num % (conf.polar_stable_every_n*conf.polar_best_inv_multip_n)) == 0))
        {
            polarity_mode = PolarityMode::polarmode_best_inv;
        }
    }

    if (polarity_mode == PolarityMode::polarmode_automatic) {
        if (branch_strategy_num > 0 &&
            conf.polar_stable_every_n > 0 &&
            ((branch_strategy_num % (conf.polar_stable_every_n*conf.polar_best_multip_n)) == 0))
        {
            polarity_mode = PolarityMode::polarmode_best;
        }
    }

    //Stable polarities only make sense in case of automatic polarities
    if (polarity_mode == PolarityMode::polarmode_automatic) {
        if (
            (branch_strategy_num > 0 &&
            conf.polar_stable_every_n > 0 &&
            ((branch_strategy_num % conf.polar_stable_every_n) == 0)) ||

            conf.polar_stable_every_n == 0 ||

            (conf.polar_stable_every_n == -1 &&
            branch_strategy == branch::vsids) ||

            (conf.polar_stable_every_n == -2 &&
            branch_strategy == branch::maple) ||

            (conf.polar_stable_every_n == -3 &&
            branch_strategy_str == "VSIDS1") ||

            (conf.polar_stable_every_n == -4 &&
            branch_strategy_str == "VSIDS2") ||

            (conf.polar_stable_every_n == -5 &&
            branch_strategy_str == "MAPLE1") ||

            (conf.polar_stable_every_n == -6 &&
            branch_strategy_str == "MAPLE2"))
        {
            polarity_mode = PolarityMode::polarmode_stable;

        }
    }

    if (conf.verbosity) {
        cout << "c [polar]"
        << " polar mode: " << getNameOfPolarmodeType(polarity_mode)
        << " branch strategy num: " << branch_strategy_num
        << " branch strategy: " << branch_strategy_str

        << endl;
    }
}

lbool Searcher::distill_clauses_if_needed()
{
    assert(decisionLevel() == 0);
    if (conf.do_distill_clauses &&
        sumConflicts > next_distill
    ) {
        if (!solver->distill_long_cls->distill(true, false)) {
            return l_False;
        }
        next_distill = std::min<double>(
            sumConflicts + sumConflicts * conf.distill_increase_conf_ratio + 7000,
            sumConflicts + conf.distill_min_confl);
    }

    return l_Undef;
}

lbool Searcher::solve(
    const uint64_t _max_confls
) {
    assert(ok);
    assert(qhead == trail.size());
    max_confl_per_search_solve_call = _max_confls;
    if (fast_backw.fast_backw_on && fast_backw.cur_max_confl == 0) {
        fast_backw.cur_max_confl = sumConflicts + fast_backw.max_confl;
    }
    num_search_called++;
    #ifdef SLOW_DEBUG
    //When asking for a lot of simple soluitons, search() gets called a lot
    check_no_removed_or_freed_cl_in_watch();
    #endif

    if (conf.verbosity >= 6) {
        cout
        << "c Searcher::solve() called"
        << endl;
    }

    resetStats();
    lbool status = l_Undef;

    set_branch_strategy(branch_strategy_num);
    setup_restart_strategy();
    check_calc_satzilla_features(true);
    check_calc_vardist_features(true);
    setup_polarity_strategy();

    while(stats.conflStats.numConflicts < max_confl_per_search_solve_call
        && status == l_Undef
    ) {
        #ifdef SLOW_DEBUG
        assert(solver->check_order_heap_sanity());
        #endif

        assert(watches.get_smudged_list().empty());
        params.clear();
        params.max_confl_to_do = max_confl_per_search_solve_call-stats.conflStats.numConflicts;
        status = search();
        if (status == l_Undef) {
            adjust_restart_strategy();
        }

        if (must_abort(status)) {
            goto end;
        }

        if (status == l_Undef && distill_clauses_if_needed() == l_False) {
            status = l_False;
            goto end;
        }
    }

    end:
    finish_up_solve(status);
    if (status == l_Undef) {
        branch_strategy_num++;
    }

    return status;
}

double Searcher::luby(double y, int x)
{
    int size = 1;
    int seq;
    for (seq = 0
        ; size < x + 1
        ; seq++
    ) {
        size = 2 * size + 1;
    }

    while (size - 1 != x) {
        size = (size - 1) >> 1;
        seq--;
        x = x % size;
    }

    return std::pow(y, seq);
}

void Searcher::setup_restart_strategy()
{
//     if (conf.verbosity) {
//         cout << "c [restart] strategy: "
//         << restart_type_to_string(cur_rest_type)
//         << endl;
//     }

    increasing_phase_size = conf.restart_first;
    max_confl_this_restart = conf.restart_first;
    switch(cur_rest_type) {
        case Restart::glue:
            params.rest_type = Restart::glue;
            break;

        case Restart::geom:
            params.rest_type = Restart::geom;
            break;

        case Restart::glue_geom:
            params.rest_type = Restart::glue;
            break;

        case Restart::luby:
            params.rest_type = Restart::luby;
            break;

        case Restart::never:
            params.rest_type = Restart::never;
            break;
    }

    print_local_restart_budget();
}

void Searcher::adjust_restart_strategy()
{
    //Haven't finished the phase. Keep rolling.
    if (max_confl_this_restart > 0)
        return;

    //Note that all of this will be overridden by params.max_confl_to_do
    switch(cur_rest_type) {
        case Restart::never:
            params.rest_type = Restart::never;
            break;

        case Restart::glue:
            params.rest_type = Restart::glue;
            break;

        case Restart::geom:
            params.rest_type = Restart::geom;
            break;

        case Restart::luby:
            params.rest_type = Restart::luby;
            break;

        case Restart::glue_geom:
            if (params.rest_type == Restart::glue) {
                params.rest_type = Restart::geom;
            } else {
                params.rest_type = Restart::glue;
            }
            break;
    }

    switch (params.rest_type) {
        //max_confl_this_restart -- for this phase of search
        //increasing_phase_size - a value that rolls and increases
        //                        it's start at conf.restart_first and never
        //                        reset
        case Restart::luby:
            max_confl_this_restart = luby(2, luby_loop_num) * (double)conf.restart_first;
            luby_loop_num++;
            break;

        case Restart::geom:
            increasing_phase_size = (double)increasing_phase_size * conf.restart_inc;
            max_confl_this_restart = increasing_phase_size;
            break;

        case Restart::glue:
            max_confl_this_restart = conf.ratio_glue_geom *increasing_phase_size;
            break;

        case Restart::never:
            max_confl_this_restart = 1000ULL*1000ULL*1000ULL;
            break;

        default:
            release_assert(false);
    }

    print_local_restart_budget();
}

inline void Searcher::print_local_restart_budget()
{
    if (conf.verbosity >= 2 || conf.print_all_restarts) {
        cout << "c [restart] at confl " << solver->sumConflicts << " -- "
        << "adjusting local restart type: "
        << std::left << std::setw(10) << getNameOfRestartType(params.rest_type)
        << " budget: " << std::setw(9) << max_confl_this_restart
        << std::right
        << " maple step_size: " << maple_step_size
        << " branching: " << std::setw(2) << branch_type_to_string(branch_strategy)
        << "   decay: "
        << std::setw(4) << std::setprecision(4) << var_decay
        << endl;
    }
}

void Searcher::check_need_restart()
{
    if ((stats.conflStats.numConflicts & 0xff) == 0xff) {
        //It's expensive to check the time all the time
        if (cpuTime() > conf.maxTime) {
            params.needToStopSearch = true;
        }

        if (must_interrupt_asap())  {
            if (conf.verbosity >= 3)
                cout << "c must_interrupt_asap() is set, restartig as soon as possible!" << endl;
            params.needToStopSearch = true;
        }
    }

    assert(params.rest_type != Restart::glue_geom);

    //dynamic
    if (params.rest_type == Restart::glue) {
        check_blocking_restart();
        if (hist.glueHist.isvalid()
            && conf.local_glue_multiplier * hist.glueHist.avg() > hist.glueHistLTLimited.avg()
        ) {
            params.needToStopSearch = true;
        }
    }

    //respect restart phase's limit
    if ((int64_t)params.conflictsDoneThisRestart > max_confl_this_restart) {
        params.needToStopSearch = true;
    }

    //respect Searcher's limit
    if (params.conflictsDoneThisRestart > params.max_confl_to_do) {
        if (conf.verbosity >= 3) {
            cout
            << "c Over limit of conflicts for this restart"
            << " -- restarting as soon as possible!" << endl;
        }
        params.needToStopSearch = true;
    }

    #ifdef VERBOSE_DEBUG
    if (params.needToStopSearch) {
        cout << "c needToStopSearch set" << endl;
    }
    #endif
}

void Searcher::print_solution_varreplace_status() const
{
    for(size_t var = 0; var < nVarsOuter(); var++) {
        if (varData[var].removed == Removed::replaced
            || varData[var].removed == Removed::elimed
        ) {
            assert(value(var) == l_Undef || varData[var].level == 0);
        }

        if (conf.verbosity >= 6
            && varData[var].removed == Removed::replaced
            && value(var) != l_Undef
        ) {
            cout
            << "var: " << var
            << " value: " << value(var)
            << " level:" << varData[var].level
            << " type: " << removed_type_to_string(varData[var].removed)
            << endl;
        }
    }
}

void Searcher::print_solution_type(const lbool status) const
{
    if (conf.verbosity >= 6) {
        if (status == l_True) {
            cout << "Solution from Searcher is SAT" << endl;
        } else if (status == l_False) {
            cout << "Solution from Searcher is UNSAT" << endl;
            cout << "OK is: " << okay() << endl;
        } else {
            cout << "Solutions from Searcher is UNKNOWN" << endl;
        }
    }
}

void Searcher::finish_up_solve(const lbool status)
{
    print_solution_type(status);
    #ifdef USE_GAUSS
    if (conf.verbosity >= 2 && status != l_Undef) {
        print_matrix_stats();
    }
    #endif

    if (status == l_True) {
        #ifdef SLOW_DEBUG
        check_order_heap_sanity();
        #endif
        assert(solver->prop_at_head());
        model = assigns;
        cancelUntil(0);
        assert(decisionLevel() == 0);

        //due to chrono BT we need to propagate once more
        PropBy confl = propagate<false>();
        assert(confl.isNULL());
        assert(solver->prop_at_head());
        #ifdef SLOW_DEBUG
        print_solution_varreplace_status();
        #endif
    } else if (status == l_False) {
        if (conflict.size() == 0) {
            ok = false;
        }
        cancelUntil(0);
        if (ok) {
            //due to chrono BT we need to propagate once more
            PropBy confl = propagate<false>();
            assert(confl.isNULL());
        }
    } else if (status == l_Undef) {
        assert(decisionLevel() == 0);
        assert(solver->prop_at_head());
    }

    stats.cpu_time = cpuTime() - startTime;
    if (conf.verbosity >= 4) {
        cout << "c Searcher::solve() finished"
        << " status: " << status
        << " numConflicts : " << stats.conflStats.numConflicts
        << " SumConfl: " << sumConflicts
        << " max_confl_per_search_solve_call:" << max_confl_per_search_solve_call
        << endl;
    }

    print_iteration_solving_stats();
}

void Searcher::print_iteration_solving_stats()
{
    if (conf.verbosity >= 3) {
        cout << "c ------ THIS ITERATION SOLVING STATS -------" << endl;
        stats.print(propStats.propagations, conf.do_print_times);
        propStats.print(stats.cpu_time);
        print_stats_line("c props/decision"
            , float_div(propStats.propagations, stats.decisions)
        );
        print_stats_line("c props/conflict"
            , float_div(propStats.propagations, stats.conflStats.numConflicts)
        );
        cout << "c ------ THIS ITERATION SOLVING STATS -------" << endl;
    }
}

inline Lit Searcher::pickBranchLit()
{
    #ifdef VERBOSE_DEBUG
    print_order_heap();
    cout << "picking decision variable, dec. level: "
    << decisionLevel() << endl;
    #endif

    uint32_t v = var_Undef;
    switch (branch_strategy) {
        case branch::vsids:
        case branch::maple:
            v = pick_var_vsids_maple();
            break;
        #ifdef VMTF_NEEDED
        case branch::vmtf:
            v = pick_var_vmtf();
            break;
        #endif
        case branch::rand: {
            v = order_heap_rand.get_random_element(mtrand);
            while (v != var_Undef && value(v) != l_Undef) {
                v = order_heap_rand.get_random_element(mtrand);
            }
            break;
        }
        default: {
            assert(false);
            exit(-1);
            break;
        }
    }

    Lit next;
    if (v != var_Undef) {
        next = Lit(v, !pick_polarity(v));
    } else {
        next = lit_Undef;
    }

    #ifdef SLOW_DEBUG
    if (next != lit_Undef) {
        assert(solver->varData[next.var()].removed == Removed::none);
    }
    #endif

    return next;
}

#ifdef VMTF_NEEDED
uint32_t Searcher::pick_var_vmtf()
{
    uint64_t searched = 0;
    uint32_t res = vmtf_queue.unassigned;
    while (res != std::numeric_limits<uint32_t>::max()
        && value(res) != l_Undef
    ) {
        res = vmtf_link(res).prev;
        searched++;
    }

    if (res == std::numeric_limits<uint32_t>::max()) {
        return var_Undef;
    }

    if (searched) {
        vmtf_update_queue_unassigned(res);
    }
    //LOG ("next queue decision variable %d vmtf_bumped %" PRId64 "", res, vmtf_bumped (res));
    return res;
}
#endif

uint32_t Searcher::pick_var_vsids_maple()
{
    Heap<VarOrderLt> &order_heap = (branch_strategy == branch::vsids) ? order_heap_vsids : order_heap_maple;
    uint32_t v = var_Undef;
    while (v == var_Undef || value(v) != l_Undef) {
        //There is no more to branch on. Satisfying assignment found.
        if (order_heap.empty()) {
            return var_Undef;
        }

        //Adjust maple to account for time passed
        if (branch_strategy == branch::maple) {
            uint32_t v2 = order_heap_maple[0];
            uint32_t age = sumConflicts - varData[v2].maple_cancelled;
            while (age > 0) {
                double decay = pow(var_decay, age);
                var_act_maple[v2].act *= decay;
                if (order_heap_maple.inHeap(v2)) {
                    order_heap_maple.increase(v2);
                }
                varData[v2].maple_cancelled = sumConflicts;
                v2 = order_heap_maple[0];
                age = sumConflicts - varData[v2].maple_cancelled;
            }
        }
        v = order_heap.removeMin();
    }
    return v;
}

void Searcher::binary_based_morem_minim(vector<Lit>& cl)
{
    int64_t limit  = more_red_minim_limit_binary_actual;
    const size_t first_n_lits_of_cl =
        std::min<size_t>(conf.max_num_lits_more_more_red_min, cl.size());
    for (size_t at_lit = 0; at_lit < first_n_lits_of_cl; at_lit++) {
        Lit lit = cl[at_lit];
        //Already removed this literal
        if (seen[lit.toInt()] == 0)
            continue;

        //Watchlist-based minimisation
        watch_subarray_const ws = watches[lit];
        for (const Watched* i = ws.begin() , *end = ws.end()
            ; i != end && limit > 0
            ; i++
        ) {
            limit--;
            if (i->isBin()) {
                if (seen[(~i->lit2()).toInt()]) {
                    stats.binTriShrinkedClause++;
                    seen[(~i->lit2()).toInt()] = 0;
                }
                continue;
            }
            break;
        }
    }
}

void Searcher::minimise_redundant_more_more(vector<Lit>& cl)
{
    stats.furtherShrinkAttempt++;
    for (const Lit lit: cl) {
        seen[lit.toInt()] = 1;
    }

    binary_based_morem_minim(cl);

    //Finally, remove the literals that have seen[literal] = 0
    //Here, we can count do stats, etc.
    bool changedClause  = false;
    vector<Lit>::iterator i = cl.begin();
    vector<Lit>::iterator j= i;

    //never remove the 0th literal -- TODO this is a bad thing
    //we should be able to remove this, but I can't figure out how to
    //reorder the clause then
    seen[cl[0].toInt()] = 1;
    for (vector<Lit>::iterator end = cl.end(); i != end; ++i) {
        if (seen[i->toInt()]) {
            *j++ = *i;
        } else {
            changedClause = true;
        }
        seen[i->toInt()] = 0;
    }
    stats.furtherShrinkedSuccess += changedClause;
    cl.resize(cl.size() - (i-j));
}

uint64_t Searcher::sumRestarts() const
{
    return stats.numRestarts + solver->get_stats().numRestarts;
}

size_t Searcher::hyper_bin_res_all(const bool check_for_set_values)
{
    size_t added = 0;

    for(std::set<BinaryClause>::const_iterator
        it = solver->needToAddBinClause.begin()
        , end = solver->needToAddBinClause.end()
        ; it != end
        ; ++it
    ) {
        lbool val1 = value(it->getLit1());
        lbool val2 = value(it->getLit2());

        if (conf.verbosity >= 6) {
            cout
            << "c Attached hyper-bin: "
            << it->getLit1() << "(val: " << val1 << " )"
            << ", " << it->getLit2() << "(val: " << val2 << " )"
            << endl;
        }

        //If binary is satisfied, skip
        if (check_for_set_values
            && (val1 == l_True || val2 == l_True)
        ) {
            continue;
        }

        if (check_for_set_values) {
            assert(val1 == l_Undef && val2 == l_Undef);
        }

        solver->attach_bin_clause(it->getLit1(), it->getLit2(), true, false);
        added++;
    }
    solver->needToAddBinClause.clear();

    return added;
}

std::pair<size_t, size_t> Searcher::remove_useless_bins(bool except_marked)
{
    size_t removedIrred = 0;
    size_t removedRed = 0;

    if (conf.doTransRed) {
        for(std::set<BinaryClause>::iterator
            it = uselessBin.begin()
            , end = uselessBin.end()
            ; it != end
            ; ++it
        ) {
            propStats.otfHyperTime += 2;
            if (conf.verbosity >= 10) {
                cout << "Removing binary clause: " << *it << endl;
            }
            propStats.otfHyperTime += solver->watches[it->getLit1()].size()/2;
            propStats.otfHyperTime += solver->watches[it->getLit2()].size()/2;
            bool removed;
            if (except_marked) {
                bool rem1 = removeWBin_except_marked(solver->watches, it->getLit1(), it->getLit2(), it->isRed());
                bool rem2 = removeWBin_except_marked(solver->watches, it->getLit2(), it->getLit1(), it->isRed());
                assert(rem1 == rem2);
                removed = rem1;
            } else {
                removeWBin(solver->watches, it->getLit1(), it->getLit2(), it->isRed());
                removeWBin(solver->watches, it->getLit2(), it->getLit1(), it->isRed());
                removed = true;
            }

            if (!removed) {
                continue;
            }

            //Update stats
            if (it->isRed()) {
                solver->binTri.redBins--;
                removedRed++;
            } else {
                solver->binTri.irredBins--;
                removedIrred++;
            }
            *drat << del << it->getLit1() << it->getLit2() << fin;

            #ifdef VERBOSE_DEBUG_FULLPROP
            cout << "Removed bin: "
            << it->getLit1() << " , " << it->getLit2()
            << " , red: " << it->isRed() << endl;
            #endif
        }
    }
    uselessBin.clear();

    return std::make_pair(removedIrred, removedRed);
}

template<bool update_bogoprops, bool red_also, bool use_disable>
PropBy Searcher::propagate() {
    const size_t origTrailSize = trail.size();

    PropBy ret;
    ret = propagate_any_order<update_bogoprops, red_also, use_disable>();

    //Drat -- If declevel 0 propagation, we have to add the unitaries
    if (decisionLevel() == 0 &&
        (drat->enabled() || conf.simulate_drat)
    ) {
        for(size_t i = origTrailSize; i < trail.size(); i++) {
            #ifdef DEBUG_DRAT
            if (conf.verbosity >= 6) {
                cout
                << "c 0-level enqueue:"
                << trail[i]
                << endl;
            }
            #endif
            *drat << add << trail[i].lit
            #ifdef STATS_NEEDED
            << 0
            << sumConflicts
            #endif
            << fin;
        }
        if (!ret.isNULL()) {
            *drat << add
            #ifdef STATS_NEEDED
            << 0
            << sumConflicts
            #endif
            << fin;
        }
    }

    return ret;
}
template PropBy Searcher::propagate<true, false, true>();
template PropBy Searcher::propagate<true, true,  true>();
template PropBy Searcher::propagate<true>();
template PropBy Searcher::propagate<false>();

size_t Searcher::mem_used() const
{
    size_t mem = HyperEngine::mem_used();
    mem += var_act_vsids.capacity()*sizeof(double);
    mem += var_act_maple.capacity()*sizeof(double);
    mem += order_heap_vsids.mem_used();
    mem += order_heap_maple.mem_used();
    mem += order_heap_rand.mem_used();
    #ifdef VMTF_NEEDED
    mem += vmtf_btab.capacity()*sizeof(uint64_t);
    mem += vmtf_links.capacity()*sizeof(Link);
    #endif
    mem += learnt_clause.capacity()*sizeof(Lit);
    mem += hist.mem_used();
    mem += conflict.capacity()*sizeof(Lit);
    mem += model.capacity()*sizeof(lbool);
    mem += analyze_stack.mem_used();
    mem += assumptions.capacity()*sizeof(Lit);

    return mem;
}

void Searcher::fill_assumptions_set()
{
    #ifdef SLOW_DEBUG
    for(auto x: varData) {
        assert(x.assumption == l_Undef);
    }
    #endif

    for(const AssumptionPair lit_pair: assumptions) {
        const Lit lit = map_outer_to_inter(lit_pair.lit_outer);
        varData[lit.var()].assumption = lit.sign() ? l_False : l_True;
    }
}

void Searcher::unfill_assumptions_set()
{
    for(const AssumptionPair lit_pair: assumptions) {
        const Lit lit = map_outer_to_inter(lit_pair.lit_outer);
        varData[lit.var()].assumption = l_Undef;
    }

    #ifdef SLOW_DEBUG
    for(auto x: varData) {
        assert(x.assumption == l_Undef);
    }
    #endif
}

void Searcher::vsids_decay_var_act()
{
    assert(branch_strategy == branch::vsids);
    var_inc_vsids *= (1.0 / var_decay);
}

void Searcher::consolidate_watches(const bool full)
{
    double t = cpuTime();
    if (full) {
        watches.full_consolidate();
    } else {
        watches.consolidate();
    }
    double time_used = cpuTime() - t;

    if (conf.verbosity) {
        cout
        << "c [consolidate] "
        << (full ? "full" : "mini")
        << conf.print_times(time_used)
        << endl;
    }

    std::stringstream ss;
    ss << "consolidate " << (full ? "full" : "mini") << " watches";
    if (sqlStats) {
        sqlStats->time_passed_min(
            solver
            , ss.str()
            , time_used
        );
    }
}

inline void Searcher::update_polarities_on_backtrack()
{
    if (polarity_mode == PolarityMode::polarmode_stable &&
        polar_stable_longest_trail_this_iter < trail.size())
    {
        for(const auto t: trail) {
            if (t.lit == lit_Undef) {
                continue;
            }
            varData[t.lit.var()].polarity = !t.lit.sign();
        }
        polar_stable_longest_trail_this_iter = trail.size();
        //cout << "polar_stable_longest_trail: " << polar_stable_longest_trail << endl;
    }

    //Just update in case it's the longest
    if (longest_trail_ever < trail.size()) {
        for(const auto t: trail) {
            if (t.lit == lit_Undef) {
                continue;
            }
            varData[t.lit.var()].best_polarity = !t.lit.sign();
        }
        longest_trail_ever = trail.size();
    }
}


//Normal running
template
void Searcher::cancelUntil<true, false>(uint32_t level);

//During inprocessing, dont update anyting really (probing, distilling)
template
void Searcher::cancelUntil<false, true>(uint32_t level);

template<bool do_insert_var_order, bool update_bogoprops>
void Searcher::cancelUntil(uint32_t blevel)
{
    #ifdef VERBOSE_DEBUG
    cout << "Canceling until level " << blevel;
    if (blevel > 0) cout << " sublevel: " << trail_lim[blevel];
    cout << endl;
    #endif

    if (decisionLevel() > blevel) {
        if (!update_bogoprops) {
            update_polarities_on_backtrack();
            #ifdef USE_GPU
            solver->datasync->unsetFromGpu(blevel);
            #endif
        }

        add_tmp_canceluntil.clear();
        #ifdef USE_GAUSS
        if (!all_matrices_disabled) {
            for (uint32_t i = 0; i < gmatrices.size(); i++) {
                if (gmatrices[i] && !gqueuedata[i].engaus_disable) {
                    //cout << "->Gauss canceling" << endl;
                    gmatrices[i]->canceling();
                } else {
                    //cout << "->Gauss NULL" << endl;
                }
            }
        }
        #endif //USE_GAUSS

        //Go through in reverse order, unassign & insert then
        //back to the vars to be branched upon
        for (int sublevel = trail.size()-1
            ; sublevel >= (int)trail_lim[blevel]
            ; sublevel--
        ) {
            #ifdef VERBOSE_DEBUG
            cout
            << "Canceling lit " << trail[sublevel].lit
            << " sublevel: " << sublevel
            << endl;
            #endif

            #ifdef ANIMATE3D
            std:cerr << "u " << var << endl;
            #endif

            const uint32_t var = trail[sublevel].lit.var();
            assert(value(var) != l_Undef);

            #ifdef STATS_NEEDED_BRANCH
            if (!update_bogoprops) {
                varData[var].last_canceled = sumConflicts;
            }
            if (!update_bogoprops && varData[var].reason == PropBy()) {
                //we want to dump & this was a decision var
                uint64_t sumConflicts_during = sumConflicts - varData[var].sumConflicts_at_picktime;
                uint64_t sumDecisions_during = sumDecisions - varData[var].sumDecisions_at_picktime;
                uint64_t sumPropagations_during = sumPropagations - varData[var].sumPropagations_at_picktime;
                uint64_t sumAntecedents_during = sumAntecedents - varData[var].sumAntecedents_at_picktime;
                uint64_t sumAntecedentsLits_during = sumAntecedentsLits - varData[var].sumAntecedentsLits_at_picktime;
                uint64_t sumConflictClauseLits_during = sumConflictClauseLits - varData[var].sumConflictClauseLits_at_picktime;
                uint64_t sumDecisionBasedCl_during = sumDecisionBasedCl - varData[var].sumDecisionBasedCl_at_picktime;
                uint64_t sumClLBD_during = sumClLBD - varData[var].sumClLBD_at_picktime;
                uint64_t sumClSize_during = sumClSize - varData[var].sumClSize_at_picktime;
                double rel_activity_at_fintime =
                    std::log2(var_act_vsids[var]+10e-300)/std::log2(max_vsids_act+10e-300);

                uint64_t inside_conflict_clause_during =
                varData[var].inside_conflict_clause - varData[var].inside_conflict_clause_at_picktime;

                uint64_t inside_conflict_clause_glue_during =
                varData[var].inside_conflict_clause_glue - varData[var].inside_conflict_clause_glue_at_picktime;

                uint64_t inside_conflict_clause_antecedents_during =
                varData[var].inside_conflict_clause_antecedents -
                varData[var].inside_conflict_clause_antecedents_at_picktime;

                if (varData[var].dump) {
                    uint64_t outer_var = map_inter_to_outer(var);

                    solver->sqlStats->var_data_fintime(
                        solver
                        , outer_var
                        , varData[var]
                        , rel_activity_at_fintime
                    );
                }

                //if STATS_NEEDED we only update for decisions, otherwise, all the time
                varData[var].sumConflicts_below_during += sumConflicts_during;
                varData[var].sumDecisions_below_during += sumDecisions_during;
                varData[var].sumPropagations_below_during += sumPropagations_during;
                varData[var].sumAntecedents_below_during += sumAntecedents_during;
                varData[var].sumAntecedentsLits_below_during += sumAntecedentsLits_during;
                varData[var].sumConflictClauseLits_below_during += sumConflictClauseLits_during;
                varData[var].sumDecisionBasedCl_below_during += sumDecisionBasedCl_during;
                varData[var].sumClLBD_below_during += sumClLBD_during;
                varData[var].sumClSize_below_during += sumClSize_during;

                varData[var].inside_conflict_clause_during +=
                inside_conflict_clause_during;

                varData[var].inside_conflict_clause_glue_during += inside_conflict_clause_glue_during;

                varData[var].inside_conflict_clause_antecedents_during +=
                inside_conflict_clause_antecedents_during;
            }
            #endif


            if (trail[sublevel].lev <= blevel) {
                add_tmp_canceluntil.push_back(trail[sublevel]);
            } else {
                if (!update_bogoprops && branch_strategy == branch::maple) {
                    assert(sumConflicts >= varData[var].maple_last_picked);
                    uint32_t age = sumConflicts - varData[var].maple_last_picked;
                    if (age > 0) {
                        //adjusted reward -> higher if conflicted more or quicker
                        double adjusted_reward = ((double)(varData[var].maple_conflicted)) / ((double)age);

                        double old_activity = var_act_maple[var].act;
                        var_act_maple[var].act =
                            maple_step_size * adjusted_reward + ((1.0 - maple_step_size ) * old_activity);

                        if (order_heap_maple.inHeap(var)) {
                            if (var_act_maple[var].act > old_activity)
                                order_heap_maple.decrease(var);
                            else
                                order_heap_maple.increase(var);
                        }
                        #ifdef VERBOSE_DEBUG
                        cout << "Adjusting reward. Var: " << var+1 << " conflicted:" << std::setprecision(12) << varData[var].maple_conflicted
                        << " old act: " << old_activity << " new act: " << var_act_maple[var] << endl
                        << " step_size: " << maple_step_size
                        << " age: " << age << " sumconflicts: " << sumConflicts << " last picked: " << varData[var].maple_last_picked
                        << endl;
                        #endif
                    }
                    varData[var].maple_cancelled = sumConflicts;
                }

                assigns[var] = l_Undef;
                if (do_insert_var_order) {
                    insert_var_order(var);
                }
            }

            #ifdef VERBOSE_DEBUG
            cout << "c Updating score by 2 for " << (trail[sublevel].lit)
            << " "  << lit_ind << endl;
            #endif
        }
        qhead = trail_lim[blevel];
        #ifdef USE_GAUSS
        gqhead = qhead;
        #endif
        trail.resize(trail_lim[blevel]);
        trail_lim.resize(blevel);

        for (int nLitId = (int)add_tmp_canceluntil.size() - 1; nLitId >= 0; --nLitId) {
            trail.push_back(add_tmp_canceluntil[nLitId]);
        }

        add_tmp_canceluntil.clear();
    }

    #ifdef VERBOSE_DEBUG
    cout << "Canceling finished. Now at level: " << decisionLevel();
    if (trail.size() > 0) {
        cout << " sublevel: " << trail.size()-1;
    }
    cout << endl;
    #endif
}

void Searcher::check_var_in_branch_strategy(uint32_t int_var) const
{
    switch(branch_strategy) {
        case branch::vsids:
            assert(order_heap_vsids.inHeap(int_var));
            break;

        case branch::maple:
            assert(order_heap_maple.inHeap(int_var));
            break;

        case branch::rand:
            assert(order_heap_rand.inHeap(int_var));
            break;

        #ifdef VMTF_NEEDED
        case branch::vmtf:
            assert(false);
            //TODO VMTF
            break;
        #endif
    }
}


ConflictData Searcher::find_conflict_level(PropBy& pb)
{
    ConflictData data;

    if (pb.getType() == PropByType::binary_t) {
        data.nHighestLevel = varData[failBinLit.var()].level;

        if (data.nHighestLevel == decisionLevel()
            && varData[pb.lit2().var()].level == decisionLevel()
        ) {
            return data;
        }

        uint32_t highestId = 0;
        // find the largest decision level in the clause
        uint32_t nLevel = varData[pb.lit2().var()].level;
        if (nLevel > data.nHighestLevel) {
            highestId = 1;
            data.nHighestLevel = nLevel;
        }

        //TODO
        // we might want to swap here if highestID is not 0
        if (highestId != 0) {
            Lit back = pb.lit2();
            pb = PropBy(failBinLit, pb.isRedStep());
            failBinLit = back;
        }

    } else {
        Lit* clause = NULL;
        uint32_t size = 0;
        ClOffset offs;
        switch(pb.getType()) {
            case PropByType::clause_t: {
                offs = pb.get_offset();
                Clause& conflCl = *cl_alloc.ptr(offs);
                clause = conflCl.getData();
                size = conflCl.size();
                break;
            }

            #ifdef USE_GAUSS
            case PropByType::xor_t: {
                vector<Lit>* cl = gmatrices[pb.get_matrix_num()]->
                    get_reason(pb.get_row_num());
                    clause = cl->data();
                    size = cl->size();
                break;
            }
            #endif

            case PropByType::binary_t:
            case PropByType::null_clause_t:
                assert(false);
                break;
        }

        data.nHighestLevel = varData[clause[0].var()].level;
        if (data.nHighestLevel == decisionLevel()
            && varData[clause[1].var()].level == decisionLevel()
        ) {
            return data;
        }

        uint32_t highestId = 0;
        // find the largest decision level in the clause
        for (uint32_t nLitId = 1; nLitId < size; ++nLitId) {
            uint32_t nLevel = varData[clause[nLitId].var()].level;
            if (nLevel > data.nHighestLevel) {
                highestId = nLitId;
                data.nHighestLevel = nLevel;
            }
        }

        if (highestId != 0) {
            std::swap(clause[0], clause[highestId]);
            if (highestId > 1 && pb.getType() == clause_t) {
                removeWCl(watches[clause[highestId]], pb.get_offset());
                watches[clause[0]].push(Watched(offs, clause[1]));
            }
        }
    }

    return data;
}

inline bool Searcher::check_order_heap_sanity() const
{
    if (conf.sampling_vars) {
        for(uint32_t outside_var: *conf.sampling_vars) {
            uint32_t outer_var = map_to_with_bva(outside_var);
            outer_var = solver->varReplacer->get_var_replaced_with_outer(outer_var);
            uint32_t int_var = map_outer_to_inter(outer_var);

            assert(varData[int_var].removed == Removed::none);

            if (int_var < nVars() &&
                varData[int_var].removed == Removed::none &&
                value(int_var) == l_Undef
            ) {
                check_var_in_branch_strategy(int_var);
            }
        }
    }

    for(size_t i = 0; i < nVars(); i++)
    {
        if (varData[i].removed == Removed::none
            && value(i) == l_Undef)
        {
            check_var_in_branch_strategy(i);
        }
    }
    assert(order_heap_vsids.heap_property());
    assert(order_heap_maple.heap_property());
    assert(order_heap_rand.heap_property());

    return true;
}

#ifdef USE_GAUSS
void Searcher::clear_gauss_matrices()
{
    xor_clauses_updated = true;
    for(uint32_t i = 0; i < gqueuedata.size(); i++) {
        auto gqd = gqueuedata[i];
        if (conf.verbosity >= 2) {
            cout
            << "c [mat" << i << "] num_props       : "
            << print_value_kilo_mega(gqd.num_props) << endl
            << "c [mat" << i << "] num_conflicts   : "
            << print_value_kilo_mega(gqd.num_conflicts)  << endl;
        }
    }

    if (conf.verbosity >= 1) {
        print_matrix_stats();
    }
    for(EGaussian* g: gmatrices) {
        delete g;
    }
    for(auto& w: gwatches) {
        w.clear();
    }
    gmatrices.clear();
    gqueuedata.clear();
}

void Searcher::print_matrix_stats()
{
    for(EGaussian* g: gmatrices) {
        if (g) {
            g->print_matrix_stats(conf.verbosity);
        }
    }
}
#endif

void Searcher::check_assumptions_sanity()
{
    for(AssumptionPair& lit_pair: assumptions) {
        Lit inter_lit = map_outer_to_inter(lit_pair.lit_outer);
        assert(inter_lit.var() < varData.size());
        assert(varData[inter_lit.var()].removed == Removed::none);
        if (varData[inter_lit.var()].assumption == l_Undef) {
            cout << "Assump " << inter_lit << " has .assumption : "
            << varData[inter_lit.var()].assumption << endl;
        }
        assert(varData[inter_lit.var()].assumption != l_Undef);
    }
}

void Searcher::bump_var_importance_all(const uint32_t var, bool only_add, double amount)
{
    vsids_bump_var_act<false>(var, amount, only_add);
    varData[var].maple_conflicted += int(2*amount);
    #ifdef VMTF_NEEDED
    vmtf_bump_queue(var);
    #endif
}


void Searcher::bump_var_importance(const uint32_t var)
{
    switch(branch_strategy) {
        case branch::vsids:
            vsids_bump_var_act<false>(var);
            break;

        case branch::maple:
            varData[var].maple_conflicted+=2;
            break;

        case branch::rand:
            break;

        #ifdef VMTF_NEEDED
        case branch::vmtf:
            vmtf_bump_queue(var);
            break;
        #endif
    }
}

void Searcher::create_new_fast_backw_assumption()
{
    //Reset conflict limit
    fast_backw.cur_max_confl = sumConflicts + fast_backw.max_confl;
    //max_confl_this_restart = params.conflictsDoneThisRestart + fast_backw.max_confl;

    //Remove indic
    const Lit indic = fast_backw._assumptions->at(fast_backw._assumptions->size()-1);
    assert(indic.sign());
    fast_backw._assumptions->pop_back();

    //Backtrack
    if (decisionLevel() >= fast_backw._assumptions->size()) {
        cancelUntil(fast_backw._assumptions->size());
    }

    //Add TRUE/FALSE duo
    uint32_t var = fast_backw.indic_to_var->at(indic.var());
    *fast_backw.test_indic = indic.var();
    *fast_backw.test_var = var;
    //cout << "Testing: " << *fast_backw.test_var << endl;
    Lit l = Lit(var, false);
    fast_backw._assumptions->push_back(l);

    Lit l2 = Lit(var+fast_backw.orig_num_vars, true);
    fast_backw._assumptions->push_back(l2);
}

lbool Searcher::new_decision_fast_backw()
{
    Lit next = lit_Undef;
    start:
    while (decisionLevel() < fast_backw._assumptions->size()) {
        // Perform user provided assumption:
        Lit p = fast_backw._assumptions->at(decisionLevel());
        p = solver->varReplacer->get_lit_replaced_with_outer(p);
        p = map_outer_to_inter(p);
        assert(varData[p.var()].removed == Removed::none);

        if (value(p) == l_True) {
            // Dummy decision level:
            new_decision_level();
        } else if (value(p) == l_False) {
            //Deal with top 2 TRUE/FALSE
//             cout << "Testing ret: " << l_False << endl;
            fast_backw._assumptions->pop_back();
            fast_backw._assumptions->pop_back();
            fast_backw.non_indep_vars->push_back(*fast_backw.test_var);

            //We reached the bottom
            if (fast_backw._assumptions->size() == fast_backw.indep_vars->size()) {
                *fast_backw.test_indic = var_Undef;
                *fast_backw.test_var = var_Undef;
                return l_True;
            }

            create_new_fast_backw_assumption();

            continue;
        } else {
            assert(p.var() < nVars());
            stats.decisionsAssump++;
            next = p;
            break;
        }
    }

    if (next == lit_Undef) {
        // New variable decision:
        next = pickBranchLit();

        //1) No decision taken, because it's SAT
        //2) We are out of conflicts
        //Either way, it's basically independent
        if (next == lit_Undef|| sumConflicts >  fast_backw.cur_max_confl) {
            if (sumConflicts >  fast_backw.cur_max_confl) {
                fast_backw.indep_because_ran_out_of_confl++;
            }
            //Let's fix this up.
            //backtrack until last.
            fast_backw._assumptions->pop_back();
            fast_backw._assumptions->pop_back();

            //Add this indic to the middle of assumptions.
            {
                vector<Lit> backup;
                backup.reserve(fast_backw._assumptions->size()+3);

                const uint32_t splice_into = fast_backw.indep_vars->size();
                for(uint32_t i = 0; i < splice_into; i++) {
                    backup.push_back(fast_backw._assumptions->at(i));
                }
                fast_backw.indep_vars->push_back(*fast_backw.test_var);
                backup.push_back(Lit(*fast_backw.test_indic, true));
                for(uint32_t i = splice_into; i < fast_backw._assumptions->size(); i++) {
                    const auto x = fast_backw._assumptions->at(i);
                    backup.push_back(x);
                }
                std::swap(*fast_backw._assumptions, backup);
                cancelUntil(splice_into);
            }

            //We reached the bottom
            if (fast_backw._assumptions->size() == fast_backw.indep_vars->size()) {
                *fast_backw.test_var = var_Undef;
                *fast_backw.test_indic = var_Undef;
                return l_True;
            }

            create_new_fast_backw_assumption();
            goto start;
        }

        //Update stats
        stats.decisions++;
        sumDecisions++;
    }

    // Increase decision level and enqueue 'next'
    assert(value(next) == l_Undef);
    new_decision_level();
    enqueue<false>(next);

    return l_Undef;
}

void Searcher::find_largest_level(Lit* lits, uint32_t count, uint32_t start)
{
    for (uint32_t i = start; i < count; i++) {
        if (value(lits[i]) == l_Undef) {
            std::swap(lits[i], lits[start]);
            return;
        }
        if (level(lits[i]) > level(lits[start])) {
            std::swap(lits[i], lits[start]);
        }
    }
}

#ifdef USE_GPU
// Learns the clause, and attach it
// May lead to canceling to backtracking if this clause leads to an implication at a level strictly lower
// than the current level
// returns a non-undef cref if this clause is in conflict
PropBy Searcher::insert_gpu_clause(Lit* lits, uint32_t count)
{
    std::sort(lits, lits+count);
    uint32_t j = 0;
    for(uint32_t i = 1; i < count; i ++) {
        if (lits[i] == lits[j]) {
            continue;
        }
        if (lits[i] == ~lits[j]) {
            //already satisfied
            return PropBy();
        }
        j++;
        lits[j] = lits[i];
    }
    count = j+1;

    //Clear it of 0-level assigned literals
    j = 0;
    for(uint32_t i = 0; i < count; i ++) {
        const Lit l = lits[i];
        if (varData[l.var()].level == 0) {
            if (value(l) == l_True) {
                return PropBy();
            }
            if (value(l) == l_False) {
                continue;
            }
        }
        lits[j] = lits[i];
        j++;
    }
    count = j;

    // if only one literal isn't false, it will be in 0 of lits
    // if only two literals aren't set, they will be in 0 and 1 of lits
    find_largest_level(lits, count, 0);
    if (count > 1) {
        find_largest_level(lits, count, 1);
    }

    //Make sure it doesn't have BVA/elimed/etc. variables
    //Make sure TRUE literal is at the beginning
    bool sat = false;
    for(uint32_t i = 0; i < count; i ++) {
        //Variable has been removed already
        if (varData[lits[i].var()].removed != Removed::none) {
            return PropBy();
        }

        //Can't deal with BVA variables
        if (varData[lits[i].var()].is_bva) {
            assert(false && "This should not be possible, other threads should not be sending BVA vars!");
            return PropBy();
        }

        if (value(lits[i]) == l_True) {
            std::swap(lits[i], lits[0]);
            sat = true;
        }
    }

//     cout << "cl-aft: ";
//     for(uint32_t i = 0; i < count; i ++) {
//         cout << lits[i] << ":" << value(lits[i]) << "(" << level(lits[i]) << ")  ";
//     }
//     cout << endl;


    //Empty clause
    if (count == 0) {
        cancelUntil(0);
        solver->ok = false;
        return PropBy();
    }

    //Unit clause
    if (count == 1) {
        /*cancelUntil(0);
        enqueue<false>(lits[0]);*/
        return PropBy();
    }

    // It's already satisfied
    // Or at least 2 are unset
    // ---> We only attach the clause
    if (sat ||
        (value(lits[0]) == l_Undef && value(lits[1]) == l_Undef) )
    {
        learn_gpu_clause(lits, count);
        return PropBy();
    }

    // All are l_False, this is a conflict
    if (value(lits[0]) == l_False)
    {
        #ifdef SLOW_DEBUG
        for (uint32_t i = 0; i < count; i++) {
            assert(value(lits[i]) == l_False);
        }
        #endif

        //Go back to highest level
        cancelUntil(level(lits[0]));

        #ifdef SLOW_DEBUG
        for (uint32_t i = 0; i < count; i++) {
            assert(value(lits[i]) == l_False);
        }
        #endif
        return learn_gpu_clause(lits, count);
    }

    // lit 0 is implied by the rest -- i.e. lit[0] is l_Undef, rest is l_False
    cancelUntil(level(lits[1]));

    #ifdef SLOW_DEBUG
    assert(value(lits[0]) == l_Undef);
    for (uint32_t i = 1; i < count; i++) {
        assert(value(lits[i]) == l_False);
    }
    #endif

    PropBy by = learn_gpu_clause(lits, count);
    enqueue<false>(lits[0], decisionLevel(), by);
    return PropBy();
}

PropBy Searcher::learn_gpu_clause(Lit* lits, uint32_t count)
{
    if (count > 2) {
        tmp_gpu_clause.resize(count);
        for(uint32_t i = 0; i < count; i++) {
            tmp_gpu_clause[i] = lits[i];
        }

        Clause* cl = cl_alloc.Clause_new(tmp_gpu_clause, sumConflicts);
        ClOffset off = cl_alloc.get_offset(cl);

        cl->stats.glue = count;
        cl->stats.which_red_array = 2;
        cl->stats.activity = 0.0f;
        cl->isRed = true;

        longRedCls[cl->stats.which_red_array].push_back(off);
        bump_cl_act<false>(cl);
        litStats.redLits += count;

        attachClause(*cl, false);
        //TODO red_stats_extra

        return PropBy(cl_alloc.get_offset(cl));
    }

    //Binary, and lits[0] may be propagated due to lits[1]
    assert(count == 2);
    attach_bin_clause(lits[0], lits[1], false, false);
    binTri.irredBins++;
    failBinLit = lits[0];
    return PropBy(lits[1], false);
}

#endif
