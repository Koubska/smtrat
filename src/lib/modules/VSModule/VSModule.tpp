/*
 *  SMT-RAT - Satisfiability-Modulo-Theories Real Algebra Toolbox
 * Copyright (C) 2012 Florian Corzilius, Ulrich Loup, Erika Abraham, Sebastian Junges
 *
 * This file is part of SMT-RAT.
 *
 * SMT-RAT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SMT-RAT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SMT-RAT.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
/**
 * File:   VSModule.cpp
 * @author Florian Corzilius <corzilius@cs.rwth-aachen.de>
 * @since 2010-05-11
 * @version 2014-11-27
 */


#include "VSModule.h"
#include "IDAllocator.h"

using namespace vs;

//#define VS_DEBUG
//#define VS_MODULE_VERBOSE_INTEGERS
//#define VS_TERMINATION_INVARIANCE

namespace smtrat
{
    template<class Settings>
    VSModule<Settings>::VSModule( ModuleType _type, const ModuleInput* _formula, RuntimeSettings*, Conditionals& _conditionals, Manager* const _manager ):
        Module( _type, _formula, _conditionals, _manager ),
        mConditionsChanged( false ),
        mInconsistentConstraintAdded( false ),
        mIDCounter( 0 ),
        #ifdef VS_STATISTICS
        mStepCounter( 0 ),
        #endif
        mpConditionIdAllocator(new IDAllocator() ),
        mpStateTree( new State( mpConditionIdAllocator, Settings::use_variable_bounds ) ),
        mAllVariables(),
        mFormulaConditionMap(),
        mRanking(),
        mVariableVector()
    {}

    template<class Settings>
    VSModule<Settings>::~VSModule()
    {
        while( !mFormulaConditionMap.empty() )
        {
            const vs::Condition* pRecCond = mFormulaConditionMap.begin()->second;
            mFormulaConditionMap.erase( mFormulaConditionMap.begin() );
            mpConditionIdAllocator->free( pRecCond->getId() );
            delete pRecCond;
            pRecCond = NULL;
        }
        delete mpStateTree;
        delete mpConditionIdAllocator;
    }

    template<class Settings>
    bool VSModule<Settings>::assertSubformula( ModuleInput::const_iterator _subformula )
    {
        Module::assertSubformula( _subformula );
        if( _subformula->formula().getType() == carl::FormulaType::CONSTRAINT )
        {
            const ConstraintT* constraint = _subformula->formula().pConstraint();
            const vs::Condition* condition = new vs::Condition( constraint, mpConditionIdAllocator->getId() );
            mFormulaConditionMap[_subformula->formula()] = condition;
            assert( constraint->isConsistent() == 2 );
            for( auto var = constraint->variables().begin(); var != constraint->variables().end(); ++var )
                mAllVariables.insert( *var );
            if( Settings::incremental_solving )
            {
                removeStatesFromRanking( *mpStateTree );
                mIDCounter = 0;
                carl::PointerSet<vs::Condition> oConds;
                oConds.insert( condition );
                std::vector<DisjunctionOfConditionConjunctions> subResults;
                DisjunctionOfConditionConjunctions subResult;

                if( Settings::int_constraints_allowed && Settings::split_neq_constraints
                    && constraint->hasIntegerValuedVariable() && !constraint->hasRealValuedVariable()
                    && constraint->relation() == carl::Relation::NEQ )
                {
                    ConditionList condVectorA;
                    condVectorA.push_back( new vs::Condition( carl::newConstraint<Poly>( constraint->lhs(), carl::Relation::LESS ), mpConditionIdAllocator->getId(), 0, false, oConds ) );
                    subResult.push_back( condVectorA );
                    ConditionList condVectorB;
                    condVectorB.push_back( new vs::Condition( carl::newConstraint<Poly>( constraint->lhs(), carl::Relation::GREATER ), mpConditionIdAllocator->getId(), 0, false, oConds ) );
                    subResult.push_back( condVectorB );
                }
                else
                {
                    ConditionList condVector;
                    condVector.push_back( new vs::Condition( constraint, mpConditionIdAllocator->getId(), 0, false, oConds ) );
                    subResult.push_back( condVector );
                }
                subResults.push_back( subResult );
                mpStateTree->addSubstitutionResults( subResults );
                addStateToRanking( mpStateTree );
                insertTooHighDegreeStatesInRanking( mpStateTree );
            }
            mConditionsChanged = true;
        }
        else if( _subformula->formula().getType() == carl::FormulaType::FALSE )
        {
            removeStatesFromRanking( *mpStateTree );
            mIDCounter = 0;
            mInfeasibleSubsets.clear();
            mInfeasibleSubsets.push_back( FormulasT() );
            mInfeasibleSubsets.back().insert( _subformula->formula() );
            mInconsistentConstraintAdded = true;
            foundAnswer( False );
            assert( checkRanking() );
            return false;
        }
        assert( checkRanking() );
        return true;
    }

    template<class Settings>
    void VSModule<Settings>::removeSubformula( ModuleInput::const_iterator _subformula )
    {
        if( _subformula->formula().getType() == carl::FormulaType::CONSTRAINT )
        {
            mInconsistentConstraintAdded = false;
            auto formulaConditionPair = mFormulaConditionMap.find( _subformula->formula() );
            assert( formulaConditionPair != mFormulaConditionMap.end() );
            const vs::Condition* condToDelete = formulaConditionPair->second;
            if( Settings::incremental_solving )
            {
                removeStatesFromRanking( *mpStateTree );
                mpStateTree->rSubResultsSimplified() = false;
                carl::PointerSet<vs::Condition> condsToDelete;
                condsToDelete.insert( condToDelete );
                mpStateTree->deleteOrigins( condsToDelete, mRanking );
                mpStateTree->rType() = State::COMBINE_SUBRESULTS;
                mpStateTree->rTakeSubResultCombAgain() = true;
                addStateToRanking( mpStateTree );
                insertTooHighDegreeStatesInRanking( mpStateTree );
            }
            mFormulaConditionMap.erase( formulaConditionPair );
            mpConditionIdAllocator->free( condToDelete->getId() );
            delete condToDelete;
            condToDelete = NULL;
            mConditionsChanged = true;
        }
        Module::removeSubformula( _subformula );
        assert( checkRanking() );
    }

    template<class Settings>
    Answer VSModule<Settings>::isConsistent()
    {
        #ifdef VS_MODULE_VERBOSE_INTEGERS
        cout << rReceivedFormula().toString( false, 0, "", true, true, true ) << endl;
        #endif
        #ifdef VS_STATISTICS
        mStepCounter = 0;
        #endif
        if( !Settings::incremental_solving )
        {
            removeStatesFromRanking( *mpStateTree );
            delete mpStateTree;
            mpStateTree = new State( mpConditionIdAllocator, Settings::use_variable_bounds );
            for( auto iter = mFormulaConditionMap.begin(); iter != mFormulaConditionMap.end(); ++iter )
            {
                carl::PointerSet<vs::Condition> oConds;
                oConds.insert( iter->second );
                std::vector<DisjunctionOfConditionConjunctions> subResults = std::vector<DisjunctionOfConditionConjunctions>();
                DisjunctionOfConditionConjunctions subResult = DisjunctionOfConditionConjunctions();
                ConditionList condVector;
                condVector.push_back( new vs::Condition( iter->first.pConstraint(), mpConditionIdAllocator->getId(), 0, false, oConds ) );
                subResult.push_back( condVector );
                subResults.push_back( subResult );
                mpStateTree->addSubstitutionResults( subResults );
            }
            addStateToRanking( mpStateTree );
        }
        if( !rReceivedFormula().isConstraintConjunction() )
            return foundAnswer( Unknown );
        if( Settings::int_constraints_allowed && !(rReceivedFormula().isIntegerConstraintConjunction() || rReceivedFormula().isRealConstraintConjunction()) )
            return foundAnswer( Unknown );
        if( !mConditionsChanged )
        {
            if( mInfeasibleSubsets.empty() )
            {
                if( solverState() == True )
                {
                    if( Settings::int_constraints_allowed && !solutionInDomain() )
                    {
                        if( Settings::branch_and_bound )
                        {
                            return foundAnswer( Unknown );
                        }
                    }
                    else
                    {
                        return consistencyTrue();
                    }
                }
                else
                {
                    return (mFormulaConditionMap.empty() ? consistencyTrue() : foundAnswer( Unknown ));
                }
            }
            else
                return foundAnswer( False );
        }
        mConditionsChanged = false;
        if( rReceivedFormula().empty() )
        {
            if( Settings::int_constraints_allowed && !solutionInDomain() )
            {
                if( Settings::branch_and_bound )
                {
                    return foundAnswer( Unknown );
                }
            }
            else
            {
                return consistencyTrue();
            }
        }
        if( mInconsistentConstraintAdded )
        {
            assert( !mInfeasibleSubsets.empty() );
            assert( !mInfeasibleSubsets.back().empty() );
            return foundAnswer( False );
        }
        if( Settings::use_variable_bounds )
        {
            if( !mpStateTree->variableBounds().isConflicting() )
            {
                std::vector<pair<vector<const ConstraintT*>, const ConstraintT*>> bDeds = mpStateTree->variableBounds().getBoundDeductions();
                for( auto bDed = bDeds.begin(); bDed != bDeds.end(); ++bDed )
                {
                    FormulasT subformulas;
                    for( auto cons = bDed->first.begin(); cons != bDed->first.end(); ++cons )
                    {
                        subformulas.insert( FormulaT( carl::FormulaType::NOT, FormulaT( *cons ) ) ); // @todo store formulas and do not generate a formula here
                    }
                    subformulas.insert( FormulaT( bDed->second ) );
    //                cout << "learn: " << deduction->toString( true, true ) << endl;
                    addDeduction( FormulaT( carl::FormulaType::OR, std::move( subformulas ) ) );
                }
            }
        }
        #ifdef VS_TERMINATION_INVARIANCE
        size_t previousId = 0;
        size_t previousValuation = 0;
        bool previousConditionsSimplified = false;
        bool previousSubResultsSimplified = false;
        bool previousTakeSubResultAgain = false;
        size_t numOfNotConsideredConditions = 0;
        #endif
        while( !mRanking.empty() )
        {
            assert( checkRanking() );
//                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if( anAnswerFound() )
                return foundAnswer( Unknown );
//            else
//                cout << "VSModule iteration" << endl;
            #ifdef VS_STATISTICS
            ++mStepCounter;
            #endif
//            if( mStepCounter >= 21520 )
//            {
//                assert(false);
//            }
//            cout << "Iteration:  " << mStepCounter << endl;
            State* currentState = mRanking.begin()->second;
            #ifdef VS_TERMINATION_INVARIANCE
            size_t tmp = 0;
            for( auto cond = currentState->conditions().begin(); cond != currentState->conditions().end(); ++cond )
                if( !(*cond)->flag() ) ++tmp;
            if( numOfNotConsideredConditions == tmp && previousId == currentState->id() && previousValuation == currentState->valuation() && previousTakeSubResultAgain == currentState->takeSubResultCombAgain() && previousConditionsSimplified == currentState->conditionsSimplified() && previousSubResultsSimplified == currentState->subResultsSimplified() )
            {
//                currentState->printAlone();
                cout << "[VS] non-termination" << endl; 
                exit( 7771 );
            }
            assert( !(numOfNotConsideredConditions == tmp && previousId == currentState->id() && previousValuation == currentState->valuation() && previousTakeSubResultAgain == currentState->takeSubResultCombAgain() && previousConditionsSimplified == currentState->conditionsSimplified() && previousSubResultsSimplified == currentState->subResultsSimplified()) );
            previousId = currentState->id();
            previousValuation = currentState->valuation();
            previousConditionsSimplified = currentState->conditionsSimplified();
            previousSubResultsSimplified = currentState->subResultsSimplified();
            previousTakeSubResultAgain = currentState->takeSubResultCombAgain();
            numOfNotConsideredConditions = tmp;
            #endif
            #ifdef VS_DEBUG
            cout << "Ranking:" << endl;
            for( auto valDTPair = mRanking.begin(); valDTPair != mRanking.end(); ++valDTPair )
            {
                stringstream stream;
                stream << "(" << valDTPair->first.first << ", " << valDTPair->first.second.first << ", " << valDTPair->first.second.second << ")";
                cout << setw(15) << stream.str();
                cout << ":  " << valDTPair->second << endl;
            }
            cout << "*** Considered state:" << endl;
            currentState->printAlone( "*** ", cout );
            #endif
            currentState->simplify( mRanking );
            #ifdef VS_DEBUG
            cout << "Simplifing results in " << endl;
            currentState->printAlone( "*** ", cout );
            #endif
            if( Settings::int_constraints_allowed && !Settings::split_neq_constraints && !currentState->isInconsistent() && !currentState->takeSubResultCombAgain() )
            {
                for( auto cond = currentState->conditions().begin(); cond != currentState->conditions().end(); ++cond )
                {
                    if( (*cond)->constraint().hasIntegerValuedVariable() && !(*cond)->constraint().hasRealValuedVariable()
                        && (*cond)->constraint().relation() == carl::Relation::NEQ )
                    {
                        // Split the neq-constraint in a preceeding sat module (make sure that it is there in your strategy when choosing this vssetting)
                        splitUnequalConstraint( FormulaT( (*cond)->pConstraint() ) );
                        assert( currentState->isRoot() );
                        return foundAnswer( Unknown );
                    }
                }
            }
            if( currentState->hasChildrenToInsert() )
            {
                currentState->rHasChildrenToInsert() = false;
                addStatesToRanking( currentState );
            }
            else
            {
                if( currentState->isInconsistent() )
                {
                    #ifdef VS_LOG_INTERMEDIATE_STEPS
                    logConditions( *currentState, false, "Intermediate_conflict_of_VSModule" );
                    #endif
                    removeStatesFromRanking( *currentState );
                    if( currentState->isRoot() )
                    {
                        updateInfeasibleSubset();
                        return foundAnswer( False );
                    }
                    else
                    {
                        currentState->passConflictToFather( Settings::check_conflict_for_side_conditions );
                        removeStateFromRanking( currentState->rFather() );
                        addStateToRanking( currentState->pFather() );
                    }
                }
                else if( currentState->takeSubResultCombAgain() && currentState->type() == State::COMBINE_SUBRESULTS )
                {
                    #ifdef VS_DEBUG
                    cout << "*** Refresh conditons:" << endl;
                    #endif
                    if( currentState->refreshConditions( mRanking ) )
                        addStateToRanking( currentState );
                    else 
                        addStatesToRanking( currentState );
                    currentState->rTakeSubResultCombAgain() = false;
                    #ifdef VS_DEBUG
                    currentState->printAlone( "   ", cout );
                    cout << "*** Conditions refreshed." << endl;
                    #endif
                }
                else if( currentState->hasRecentlyAddedConditions() )//&& !(currentState->takeSubResultCombAgain() && currentState->isRoot() ) )
                {
                    #ifdef VS_DEBUG
                    cout << "*** Propagate new conditions :" << endl;
                    #endif
                    propagateNewConditions(currentState);
                    #ifdef VS_DEBUG
                    cout << "*** Propagate new conditions ready." << endl;
                    #endif
                }
                else
                {
                    #ifdef SMTRAT_VS_VARIABLEBOUNDS
                    if( !currentState->checkTestCandidatesForBounds() )
                    {
                        currentState->rInconsistent() = true;
                        removeStatesFromRanking( *currentState );
                    }
                    else
                    {
                        #endif
                        switch( currentState->type() )
                        {
                            case State::SUBSTITUTION_TO_APPLY:
                            {
                                #ifdef VS_DEBUG
                                cout << "*** SubstituteAll changes it to:" << endl;
                                #else
                                #ifdef VS_MODULE_VERBOSE_INTEGERS
                                bool minf = currentState->substitution().type() == Substitution::MINUS_INFINITY;
                                if( !minf )
                                {
                                    cout << string( currentState->treeDepth()*3, ' ') << "Test candidate  " << endl;
                                    currentState->substitution().print( true, false, cout, string( currentState->treeDepth()*3, ' '));
                                }
                                #endif
                                #endif
                                if( !substituteAll( currentState, currentState->rFather().rConditions() ) )
                                {
                                    // Delete the currently considered state.
                                    assert( currentState->rInconsistent() );
                                    removeStateFromRanking( *currentState );
                                }
                                #ifndef VS_DEBUG
                                #ifdef VS_MODULE_VERBOSE_INTEGERS
                                if( minf )
                                {
                                    cout << string( currentState->treeDepth()*3, ' ') << "Test candidate  [from -inf]" << endl;
                                    currentState->substitution().print( true, false, cout, string( currentState->treeDepth()*3, ' '));
                                }
                                #endif
                                #endif
                                #ifdef VS_DEBUG
                                cout << "*** SubstituteAll ready." << endl;
                                #endif
                                break;
                            }
                            case State::COMBINE_SUBRESULTS:
                            {
                                #ifdef VS_DEBUG
                                cout << "*** Refresh conditons:" << endl;
                                #endif
                                if( currentState->nextSubResultCombination() )
                                {
                                    if( currentState->refreshConditions( mRanking ) )
                                        addStateToRanking( currentState );
                                    else 
                                        addStatesToRanking( currentState );
                                    #ifdef VS_DEBUG
                                    currentState->printAlone( "   ", cout );
                                    #endif
                                }
                                else
                                {
                                    // If it was the last combination, delete the state.
                                    currentState->rInconsistent() = true;
                                    removeStatesFromRanking( *currentState );
                                    currentState->rFather().rMarkedAsDeleted() = false;
                                    addStateToRanking( currentState->pFather() );

                                }
                                #ifdef VS_DEBUG
                                cout << "*** Conditions refreshed." << endl;
                                #endif
                                break;
                            }
                            case State::TEST_CANDIDATE_TO_GENERATE:
                            {
                                // Set the index, if not already done, to the best variable to eliminate next.
                                if( currentState->index() == carl::Variable::NO_VARIABLE ) 
                                    currentState->initIndex( mAllVariables, Settings::prefer_equation_over_all );
                                else if( currentState->tryToRefreshIndex() )
                                {
                                    if( currentState->initIndex( mAllVariables, Settings::prefer_equation_over_all ) )
                                    {
                                        currentState->initConditionFlags();
                                        currentState->resetConflictSets();
                                        while( !currentState->children().empty() )
                                        {
                                            State* toDelete = currentState->rChildren().back();
                                            removeStatesFromRanking( *toDelete );
                                            currentState->rChildren().pop_back();
                                            currentState->resetInfinityChild( toDelete );
                                            delete toDelete;  // DELETE STATE
                                        }
                                        currentState->updateIntTestCandidates();
                                    }
                                }
                                // Find the most adequate conditions to continue.
                                const vs::Condition* currentCondition;
                                if( !currentState->bestCondition( currentCondition, mAllVariables.size(), Settings::prefer_equation_over_all ) )
                                {
                                    if( !(*currentState).cannotBeSolved() && currentState->tooHighDegreeConditions().empty() )
                                    {
                                        // It is a state, where no more elimination could be applied to the conditions.
                                        if( Settings::int_constraints_allowed && !Settings::branch_and_bound && currentState->index().getType() == carl::VariableType::VT_INT )
                                        {
                                            if( !currentState->hasInfinityChild() )
                                            {
                                                if( !Settings::use_variable_bounds || currentState->variableBounds().getDoubleInterval( currentState->index() ).lowerBoundType() == carl::BoundType::INFTY )
                                                {
                                                    // Create state ( Conditions, [x -> -infinity]):
                                                    carl::PointerSet<vs::Condition> oConditions;
                                                    for( auto cond : currentState->conditions() )
                                                        oConditions.insert( cond );
                                                    Substitution sub = Substitution( currentState->index(), Substitution::MINUS_INFINITY, oConditions );
                                                    std::vector<State*> addedChildren = currentState->addChild( sub );
                                                    if( !addedChildren.empty() )
                                                    {
                                                        // Add its valuation to the current ranking.
                                                        currentState->setInfinityChild( currentState->rChildren().back() );
                                                        while( !addedChildren.empty() )
                                                        {
                                                            addStatesToRanking( addedChildren.back() );
                                                            addedChildren.pop_back();
                                                        }
                                                        #ifdef VS_DEBUG
                                                        currentState->rChildren().back()->print( "   ", cout );
                                                        #endif
                                                    }
                                                }
                                            }
                                        }
                                        if( currentState->conditions().empty() )
                                        {
                                            #ifdef VS_DEBUG
                                            cout << "*** Check ancestors!" << endl;
                                            #endif
                                            // Check if there are still conditions in any ancestor, which have not been considered.
                                            State * unfinishedAncestor;
                                            if( currentState->unfinishedAncestor( unfinishedAncestor ) )
                                            {
                                                // Go back to this ancestor and refine.
                                                removeStatesFromRanking( *unfinishedAncestor );
                                                if( !unfinishedAncestor->subResultsSimplified() )
                                                {
                                                    unfinishedAncestor->print();
                                                }
                                                unfinishedAncestor->extendSubResultCombination();
                                                unfinishedAncestor->rType() = State::COMBINE_SUBRESULTS;
                                                if( unfinishedAncestor->refreshConditions( mRanking ) ) 
                                                    addStateToRanking( unfinishedAncestor );
                                                else 
                                                    addStatesToRanking( unfinishedAncestor );
                                                #ifdef VS_DEBUG
                                                cout << "*** Found an unfinished ancestor:" << endl;
                                                unfinishedAncestor->printAlone();
                                                #endif
                                            }
                                            else // Solution.
                                            {
                                                if( Settings::int_constraints_allowed && !solutionInDomain() )
                                                {
                                                    if( Settings::branch_and_bound )
                                                    {
                                                        return foundAnswer( Unknown );
                                                    }
                                                }
                                                else
                                                {
                                                    return consistencyTrue();
                                                }
                                            }
                                        }
                                        // It is a state, where all conditions have been used for test candidate generation.
                                        else
                                        {
                                            // Check whether there are still test candidates in form of children left.
                                            bool currentStateHasChildrenToConsider = false;
                                            bool currentStateHasChildrenWithToHighDegree = false;
                                            auto child = currentState->rChildren().begin();
                                            while( child != currentState->children().end() )
                                            {
                                                if( !(**child).isInconsistent() )
                                                {
                                                    if( !(**child).markedAsDeleted() )
                                                        addStateToRanking( *child );
                                                    if( !(**child).cannotBeSolved() && !(**child).markedAsDeleted() )
                                                        currentStateHasChildrenToConsider = true;
                                                    else 
                                                        currentStateHasChildrenWithToHighDegree = true;
                                                }
                                                child++;
                                            }

                                            if( !currentStateHasChildrenToConsider )
                                            {
                                                if( !currentStateHasChildrenWithToHighDegree )
                                                {
                                                    currentState->rInconsistent() = true;
                                                    #ifdef VS_LOG_INTERMEDIATE_STEPS
                                                    logConditions( *currentState, false, "Intermediate_conflict_of_VSModule" );
                                                    #endif
                                                    removeStatesFromRanking( *currentState );
                                                    if( currentState->isRoot() )
                                                        updateInfeasibleSubset();
                                                    else
                                                    {
                                                        currentState->passConflictToFather( Settings::check_conflict_for_side_conditions );
                                                        removeStateFromRanking( currentState->rFather() );
                                                        addStateToRanking( currentState->pFather() );
                                                    }
                                                }
                                                else
                                                {
                                                    currentState->rMarkedAsDeleted() = true;
                                                    removeStateFromRanking( *currentState );
                                                }
                                            }
                                        }
                                    }
                                    else
                                    {
                                        if( (*currentState).cannotBeSolved() )
                                        {
                                            // If we need to involve another approach.
                                            Answer result = runBackendSolvers( currentState );
                                            switch( result )
                                            {
                                                case True:
                                                {
                                                    currentState->rCannotBeSolved() = true;
                                                    State * unfinishedAncestor;
                                                    if( currentState->unfinishedAncestor( unfinishedAncestor ) )
                                                    {
                                                        // Go back to this ancestor and refine.
                                                        removeStatesFromRanking( *unfinishedAncestor );
                                                        unfinishedAncestor->extendSubResultCombination();
                                                        unfinishedAncestor->rType() = State::COMBINE_SUBRESULTS;
                                                        if( unfinishedAncestor->refreshConditions( mRanking ) )
                                                            addStateToRanking( unfinishedAncestor );
                                                        else
                                                            addStatesToRanking( unfinishedAncestor );
                                                    }
                                                    else // Solution.
                                                    {
                                                        if( Settings::int_constraints_allowed && !solutionInDomain() )
                                                        {
                                                            if( Settings::branch_and_bound )
                                                            {
                                                                return foundAnswer( Unknown );
                                                            }
                                                        }
                                                        else
                                                        {
                                                            return consistencyTrue();
                                                        }
                                                    }
                                                    break;
                                                }
                                                case False:
                                                {
                                                    break;
                                                }
                                                case Unknown:
                                                {
                                                    return foundAnswer( Unknown );
                                                }
                                                default:
                                                {
                                                    cout << "Error: Unknown answer in method " << __func__ << " line " << __LINE__ << endl;
                                                    return foundAnswer( Unknown );
                                                }
                                            }
                                        }
                                        else
                                        {
//                                            return foundAnswer( Unknown );
                                            currentState->rCannotBeSolved() = true;
                                            addStateToRanking( currentState );
                                        }
                                    }
                                }
                                // Generate test candidates for the chosen variable and the chosen condition.
                                else
                                {
                                    if( Settings::local_conflict_search && Settings::int_constraints_allowed
                                        && currentState->index().getType() == carl::VariableType::VT_REAL && currentState->hasLocalConflict() )
                                    {
                                        removeStatesFromRanking( *currentState );
                                        addStateToRanking( currentState );
                                    }
                                    else
                                    {
                                        #ifdef VS_DEBUG
                                        cout << "*** Eliminate " << currentState->index() << " in ";
                                        cout << currentCondition->constraint().toString( 0, true, true );
                                        cout << " creates:" << endl;
                                        #endif
                                        eliminate( currentState, currentState->index(), currentCondition );
                                        #ifdef VS_DEBUG
                                        cout << "*** Eliminate ready." << endl;
                                        #endif
                                    }
                                }
                                break;
                            }
                            default:
                                assert( false );
                        }
                        #ifdef SMTRAT_VS_VARIABLEBOUNDS
                    }
                    #endif
                }
            }
        }
        #ifdef VS_LOG_INTERMEDIATE_STEPS
        if( mpStateTree->conflictSets().empty() ) logConditions( *mpStateTree, false, "Intermediate_conflict_of_VSModule" );
        #endif
        assert( !mpStateTree->conflictSets().empty() );
        updateInfeasibleSubset();
        #ifdef VS_DEBUG
        printAll();
        #endif
        return foundAnswer( False );
    }

    template<class Settings>
    void VSModule<Settings>::updateModel() const
    {
        clearModel();
        if( solverState() == True )
        {
            if( mFormulaConditionMap.empty() )
                return;
            for( size_t i = mVariableVector.size(); i<=mRanking.begin()->second->treeDepth(); ++i )
            {
                stringstream outA;
                outA << "m_inf_" << id() << "_" << i;
                carl::Variable minfVar( carl::freshRealVariable( outA.str() ) );
                stringstream outB;
                outB << "eps_" << id() << "_" << i;
                carl::Variable epsVar( carl::freshRealVariable( outB.str() ) );
                mVariableVector.push_back( std::pair<carl::Variable,carl::Variable>( minfVar, epsVar ) );
            }
            assert( !mRanking.empty() );
            carl::Variables allVarsInRoot;
            mpStateTree->variables( allVarsInRoot );
            const State* state = mRanking.begin()->second;
            while( !state->isRoot() )
            {
                const Substitution& sub = state->substitution();
                ModelValue ass;
                if( sub.type() == Substitution::MINUS_INFINITY )
                    ass = SqrtEx( Poly( mVariableVector.at( state->treeDepth()-1 ).first ) );
                else
                {
                    assert( sub.type() != Substitution::PLUS_INFINITY );
                    if( state->substitution().variable().getType() == carl::VariableType::VT_INT )
                    {
                        Rational valueRational;
                        sub.term().evaluate( valueRational, getIntervalAssignment( state ), 0 );
                        ass = SqrtEx( Poly( valueRational ) );
                    }
                    else
                    {
                        ass = SqrtEx( sub.term() );
                        if( sub.type() == Substitution::PLUS_EPSILON )
                            ass = ass.asSqrtEx() + SqrtEx( Poly( mVariableVector.at( state->treeDepth()-1 ).second ) );
                    }
                }
                mModel.insert(std::make_pair(state->substitution().variable(), ass));
                state = state->pFather();
            }
            if( mRanking.begin()->second->cannotBeSolved() )
                Module::getBackendsModel();
            // All variables which occur in the root of the constructed state tree but were incidentally eliminated
            // (during the elimination of another variable) can have an arbitrary assignment. If the variable has the
            // real domain, we leave at as a parameter, and, if it has the integer domain we assign 0 to it.
            for( auto var = allVarsInRoot.begin(); var != allVarsInRoot.end(); ++var )
            {
                ModelValue ass;
                ass = SqrtEx( var->getType() == carl::VariableType::VT_INT ? ZERO_POLYNOMIAL : Poly( *var ) );
                // Note, that this assignment won't take effect if the variable got an assignment by a backend module.
                mModel.insert(std::make_pair(*var, ass));
            }
        }
    }
    
    template<class Settings>
    Answer VSModule<Settings>::consistencyTrue()
    {
        #ifdef VS_LOG_INTERMEDIATE_STEPS
        checkAnswer();
        #endif
        #ifdef VS_PRINT_ANSWERS
        printAnswer();
        #endif
        #ifdef VS_DEBUG
        printAll();
        #endif
        return foundAnswer( True );
    }

    template<class Settings>
    void VSModule<Settings>::eliminate( State* _currentState, const carl::Variable& _eliminationVar, const vs::Condition* _condition )
    {
        // Get the constraint of this condition.
        const ConstraintT* constraint = (*_condition).pConstraint();
        assert( _condition->constraint().hasVariable( _eliminationVar ) );
        bool generatedTestCandidateBeingASolution = false;
        unsigned numberOfAddedChildren = 0;
        carl::PointerSet<vs::Condition> oConditions;
        oConditions.insert( _condition );
        #ifdef SMTRAT_VS_VARIABLEBOUNDS
        if( !Settings::use_variable_bounds || _currentState->hasRootsInVariableBounds( _condition, Settings::sturm_sequence_for_root_check ) )
        {
            #endif
            carl::Relation relation = (*_condition).constraint().relation();
            if( !Settings::use_strict_inequalities_for_test_candidate_generation )
            {
                if( relation == carl::Relation::LESS || relation == carl::Relation::GREATER || relation == carl::Relation::NEQ )
                {
                    _currentState->rTooHighDegreeConditions().insert( _condition );
                    _condition->rFlag() = true;
                    return;
                }
            }
            // Determine the substitution type: normal or +epsilon
            bool weakConstraint = (relation == carl::Relation::EQ || relation == carl::Relation::LEQ || relation == carl::Relation::GEQ);
            Substitution::Type subType = weakConstraint ? Substitution::NORMAL : Substitution::PLUS_EPSILON;
            std::vector< Poly > factors = std::vector< Poly >();
            carl::PointerSet<ConstraintT> sideConditions;
            if( Settings::elimination_with_factorization && constraint->hasFactorization() )
            {
                for( auto iter = constraint->factorization().begin(); iter != constraint->factorization().end(); ++iter )
                {
                    carl::Variables factorVars;
                    iter->first.gatherVariables( factorVars );
                    if( factorVars.find( _eliminationVar ) != factorVars.end() )
                        factors.push_back( iter->first );
                    else
                    {
                        const smtrat::ConstraintT* cons = carl::newConstraint<Poly>( iter->first, carl::Relation::NEQ );
                        if( cons != carl::constraintPool<Poly>().consistentConstraint() )
                        {
                            assert( cons != carl::constraintPool<Poly>().inconsistentConstraint() );
                            sideConditions.insert( cons );
                        }
                    }
                }
            }
            else
                factors.push_back( constraint->lhs() );
            for( auto factor = factors.begin(); factor != factors.end(); ++factor )
            {
                #ifdef VS_DEBUG
                cout << "Eliminate for " << *factor << endl;
                #endif
                VarPolyInfo varInfo = factor->getVarInfo<true>( _eliminationVar );
                const std::map<unsigned, Poly>& coeffs = varInfo.coeffs();
                assert( !coeffs.empty() );
                // Generate test candidates for the chosen variable considering the chosen constraint.
                switch( coeffs.rbegin()->first )
                {
                    case 0:
                    {
                        assert( false );
                        break;
                    }
                    //degree = 1
                    case 1:
                    {
                        Poly constantCoeff;
                        auto iter = coeffs.find( 0 );
                        if( iter != coeffs.end() ) constantCoeff = iter->second;
                        // Create state ({b!=0} + oldConditions, [x -> -c/b]):
                        const smtrat::ConstraintT* cons = carl::newConstraint<Poly>( coeffs.rbegin()->second, carl::Relation::NEQ );
                        if( cons == carl::constraintPool<Poly>().inconsistentConstraint() )
                        {
                            if( relation == carl::Relation::EQ )
                                generatedTestCandidateBeingASolution = sideConditions.empty();
                        }
                        else
                        {
                            carl::PointerSet<ConstraintT> sideCond = sideConditions;
                            if( cons != carl::constraintPool<Poly>().consistentConstraint() )
                                sideCond.insert( cons );
                            SqrtEx sqEx = SqrtEx( -constantCoeff, ZERO_POLYNOMIAL, coeffs.rbegin()->second, ZERO_POLYNOMIAL );
                            Substitution sub = Substitution( _eliminationVar, sqEx, subType, oConditions, sideCond );
                            std::vector<State*> addedChildren = _currentState->addChild( sub );
                            if( !addedChildren.empty() )
                            {
                                if( relation == carl::Relation::EQ && !_currentState->children().back()->hasSubstitutionResults() )
                                {
                                    _currentState->rChildren().back()->setOriginalCondition( _condition );
                                    generatedTestCandidateBeingASolution = true;
                                }
                                // Add its valuation to the current ranking.
                                while( !addedChildren.empty() )
                                {
                                    addStatesToRanking( addedChildren.back() );
                                    addedChildren.pop_back();
                                }
                                ++numberOfAddedChildren;
                                #ifdef VS_DEBUG
                                (*(*_currentState).rChildren().back()).print( "   ", cout );
                                #endif
                            }
                        }
                        break;
                    }
                    //degree = 2
                    case 2:
                    {
                        Poly constantCoeff;
                        auto iter = coeffs.find( 0 );
                        if( iter != coeffs.end() ) constantCoeff = iter->second;
                        Poly linearCoeff;
                        iter = coeffs.find( 1 );
                        if( iter != coeffs.end() ) linearCoeff = iter->second;
                        Poly radicand = linearCoeff.pow( 2 ) - Rational( 4 ) * coeffs.rbegin()->second * constantCoeff;
                        bool constraintHasZeros = false;
                        const smtrat::ConstraintT* cons11 = carl::newConstraint<Poly>( coeffs.rbegin()->second, carl::Relation::EQ );
                        if( cons11 != carl::constraintPool<Poly>().inconsistentConstraint() )
                        {
                            // Create state ({a==0, b!=0} + oldConditions, [x -> -c/b]):
                            const smtrat::ConstraintT* cons12 = carl::newConstraint<Poly>( linearCoeff, carl::Relation::NEQ );
                            if( cons12 != carl::constraintPool<Poly>().inconsistentConstraint() )
                            {
                                carl::PointerSet<ConstraintT> sideCond = sideConditions;
                                if( cons11 != carl::constraintPool<Poly>().consistentConstraint() )
                                    sideCond.insert( cons11 );
                                if( cons12 != carl::constraintPool<Poly>().consistentConstraint() )
                                    sideCond.insert( cons12 );
                                SqrtEx sqEx = SqrtEx( -constantCoeff, ZERO_POLYNOMIAL, linearCoeff, ZERO_POLYNOMIAL );
                                Substitution sub = Substitution( _eliminationVar, sqEx, subType, oConditions, sideCond );
                                std::vector<State*> addedChildren = _currentState->addChild( sub );
                                if( !addedChildren.empty() )
                                {
                                    if( relation == carl::Relation::EQ && !_currentState->children().back()->hasSubstitutionResults() )
                                    {
                                        _currentState->rChildren().back()->setOriginalCondition( _condition );
                                        generatedTestCandidateBeingASolution = true;
                                    }
                                    // Add its valuation to the current ranking.
                                    while( !addedChildren.empty() )
                                    {
                                        addStatesToRanking( addedChildren.back() );
                                        addedChildren.pop_back();
                                    }
                                    ++numberOfAddedChildren;
                                    #ifdef VS_DEBUG
                                    (*(*_currentState).rChildren().back()).print( "   ", cout );
                                    #endif
                                }
                                constraintHasZeros = true;
                            }
                        }
                        const smtrat::ConstraintT* cons21 = carl::newConstraint<Poly>( radicand, carl::Relation::GEQ );
                        if( cons21 != carl::constraintPool<Poly>().inconsistentConstraint() )
                        {
                            const smtrat::ConstraintT* cons22 = carl::newConstraint<Poly>( coeffs.rbegin()->second, carl::Relation::NEQ );
                            if( cons22 != carl::constraintPool<Poly>().inconsistentConstraint() )
                            {
                                carl::PointerSet<ConstraintT> sideCond = sideConditions;
                                if( cons21 != carl::constraintPool<Poly>().consistentConstraint() )
                                    sideCond.insert( cons21 );
                                if( cons22 != carl::constraintPool<Poly>().consistentConstraint() )
                                    sideCond.insert( cons22 );
                                // Create state ({a!=0, b^2-4ac>=0} + oldConditions, [x -> (-b+sqrt(b^2-4ac))/2a]):
                                SqrtEx sqExA = SqrtEx( -linearCoeff, ONE_POLYNOMIAL, Rational( 2 ) * coeffs.rbegin()->second, radicand );
                                Substitution subA = Substitution( _eliminationVar, sqExA, subType, oConditions, sideCond );
                                std::vector<State*> addedChildrenA = _currentState->addChild( subA );
                                if( !addedChildrenA.empty() )
                                {
                                    if( relation == carl::Relation::EQ && !_currentState->children().back()->hasSubstitutionResults() )
                                    {
                                        _currentState->rChildren().back()->setOriginalCondition( _condition );
                                        generatedTestCandidateBeingASolution = true;
                                    }
                                    // Add its valuation to the current ranking.
                                    while( !addedChildrenA.empty() )
                                    {
                                        addStatesToRanking( addedChildrenA.back() );
                                        addedChildrenA.pop_back();
                                    }
                                    ++numberOfAddedChildren;
                                    #ifdef VS_DEBUG
                                    (*(*_currentState).rChildren().back()).print( "   ", cout );
                                    #endif
                                }
                                // Create state ({a!=0, b^2-4ac>=0} + oldConditions, [x -> (-b-sqrt(b^2-4ac))/2a]):
                                SqrtEx sqExB = SqrtEx( -linearCoeff, MINUS_ONE_POLYNOMIAL, Rational( 2 ) * coeffs.rbegin()->second, radicand );
                                Substitution subB = Substitution( _eliminationVar, sqExB, subType, oConditions, sideCond );
                                std::vector<State*> addedChildrenB = _currentState->addChild( subB );
                                if( !addedChildrenB.empty() )
                                {
                                    if( relation == carl::Relation::EQ && !_currentState->children().back()->hasSubstitutionResults() )
                                    {
                                        _currentState->rChildren().back()->setOriginalCondition( _condition );
                                        generatedTestCandidateBeingASolution = true;
                                    }
                                    // Add its valuation to the current ranking.
                                    while( !addedChildrenB.empty() )
                                    {
                                        addStatesToRanking( addedChildrenB.back() );
                                        addedChildrenB.pop_back();
                                    }
                                    ++numberOfAddedChildren;
                                    #ifdef VS_DEBUG
                                    (*(*_currentState).rChildren().back()).print( "   ", cout );
                                    #endif
                                }
                                constraintHasZeros = true;
                            }
                        }
                        if( !constraintHasZeros && relation == carl::Relation::EQ )
                            generatedTestCandidateBeingASolution = sideConditions.empty();
                        break;
                    }
                    //degree > 2 (> 3)
                    default:
                    {
                        _currentState->rTooHighDegreeConditions().insert( _condition );
                        break;
                    }
                }
            }
        #ifdef SMTRAT_VS_VARIABLEBOUNDS
        }
        #endif
        if( !Settings::int_constraints_allowed || Settings::branch_and_bound || _eliminationVar.getType() != carl::VariableType::VT_INT )
        {
            if( !generatedTestCandidateBeingASolution && !_currentState->isInconsistent() )
            {
                // Create state ( Conditions, [x -> -infinity]):
                Substitution sub = Substitution( _eliminationVar, Substitution::MINUS_INFINITY, oConditions );
                std::vector<State*> addedChildren = _currentState->addChild( sub );
                if( !addedChildren.empty() )
                {
                    // Add its valuation to the current ranking.
                    while( !addedChildren.empty() )
                    {
                        addStatesToRanking( addedChildren.back() );
                        addedChildren.pop_back();
                    }
                    numberOfAddedChildren++;
                    #ifdef VS_DEBUG
                    (*(*_currentState).rChildren().back()).print( "   ", cout );
                    #endif
                }
            }
        }
        if( Settings::int_constraints_allowed && Settings::branch_and_bound && _eliminationVar.getType() == carl::VariableType::VT_INT )
        {
            if( !generatedTestCandidateBeingASolution && !_currentState->isInconsistent() )
            {
                // Create state ( Conditions, [x -> -infinity]):
                Substitution sub = Substitution( _eliminationVar, Substitution::PLUS_INFINITY, oConditions );
                std::vector<State*> addedChildren = _currentState->addChild( sub );
                if( !addedChildren.empty() )
                {
                    // Add its valuation to the current ranking.
                    while( !addedChildren.empty() )
                    {
                        addStatesToRanking( addedChildren.back() );
                        addedChildren.pop_back();
                    }
                    numberOfAddedChildren++;
                    #ifdef VS_DEBUG
                    (*(*_currentState).rChildren().back()).print( "   ", cout );
                    #endif
                }
            }
        }
        if( generatedTestCandidateBeingASolution )
        {
            _currentState->rTooHighDegreeConditions().clear();
            for( auto cond = _currentState->rConditions().begin(); cond != _currentState->conditions().end(); ++cond )
                (**cond).rFlag() = true;
            assert( numberOfAddedChildren <= _currentState->children().size() );
            while( _currentState->children().size() > numberOfAddedChildren )
            {
                State* toDelete = *_currentState->rChildren().begin();
                removeStatesFromRanking( *toDelete );
                _currentState->resetConflictSets();
                _currentState->rChildren().erase( _currentState->rChildren().begin() );
                _currentState->resetInfinityChild( toDelete );
                delete toDelete;  // DELETE STATE
            }
            _currentState->updateIntTestCandidates();
            if( numberOfAddedChildren == 0 )
            {
                ConditionSetSet conflictSet;
                carl::PointerSet<vs::Condition> condSet;
                condSet.insert( _condition );
                conflictSet.insert( condSet );
                _currentState->addConflicts( NULL, conflictSet );
                _currentState->rInconsistent() = true;
            }
        }
        else
            (*_condition).rFlag() = true;
        addStateToRanking( _currentState );
    }

    template<class Settings>
    bool VSModule<Settings>::substituteAll( State* _currentState, ConditionList& _conditions )
    {
        /*
         * Create a vector to store the results of each single substitution. Each entry corresponds to
         * the results of a single substitution. These results can be considered as a disjunction of
         * conjunctions of constraints.
         */
        std::vector<DisjunctionOfConditionConjunctions> allSubResults;
        // The substitution to apply.
        assert( !_currentState->isRoot() );
        const Substitution& currentSubs = _currentState->substitution();
        // The variable to substitute.
        const carl::Variable& substitutionVariable = currentSubs.variable();
        // The conditions of the currently considered state, without the one getting just eliminated.
        ConditionList oldConditions;
        bool anySubstitutionFailed = false;
        bool allSubstitutionsApplied = true;
        ConditionSetSet conflictSet;
        #ifdef SMTRAT_VS_VARIABLEBOUNDS
        if( _currentState->father().variableBounds().isConflicting() )
        {
            _currentState->father().printAlone();
            _currentState->printAlone();
        }
//        EvalDoubleIntervalMap solBox = (currentSubs.type() == Substitution::MINUS_INFINITY ? EvalDoubleIntervalMap() : _currentState->rFather().rVariableBounds().getIntervalMap());
        EvalDoubleIntervalMap solBox = _currentState->father().variableBounds().getIntervalMap();
        #else
        EvalDoubleIntervalMap solBox = EvalDoubleIntervalMap();
        #endif
        // Apply the substitution to the given conditions.
        for( auto cond = _conditions.begin(); cond != _conditions.end(); ++cond )
        {
            // The constraint to substitute in.
            const ConstraintT* currentConstraint = (**cond).pConstraint();
            // Does the condition contain the variable to substitute.
            auto var = currentConstraint->variables().find( substitutionVariable );
            if( var == currentConstraint->variables().end() )
            {
                if( !anySubstitutionFailed )
                {
                    oldConditions.push_back( new vs::Condition( currentConstraint, mpConditionIdAllocator->getId(), (**cond).valuation() ) );
                    oldConditions.back()->pOriginalConditions()->insert( *cond );
                }
            }
            else
            {
                DisjunctionOfConstraintConjunctions subResult;
                carl::Variables conflVars;
                bool substitutionCouldBeApplied = substitute( currentConstraint, currentSubs, subResult, Settings::virtual_substitution_according_paper, conflVars, solBox );
                allSubstitutionsApplied &= substitutionCouldBeApplied;
                // Create the the conditions according to the just created constraint prototypes.
                if( substitutionCouldBeApplied && subResult.empty() )
                {
                    anySubstitutionFailed = true;
                    carl::PointerSet<vs::Condition> condSet;
                    condSet.insert( *cond );
                    if( _currentState->pOriginalCondition() != NULL )
                        condSet.insert( _currentState->pOriginalCondition() );
                    #ifdef SMTRAT_VS_VARIABLEBOUNDS
                    carl::PointerSet<vs::Condition> conflictingBounds = _currentState->father().variableBounds().getOriginsOfBounds( conflVars );
                    condSet.insert( conflictingBounds.begin(), conflictingBounds.end() );
                    #endif
                    conflictSet.insert( condSet );
                }
                else
                {
                    if( allSubstitutionsApplied && !anySubstitutionFailed )
                    {
                        allSubResults.push_back( DisjunctionOfConditionConjunctions() );
                        DisjunctionOfConditionConjunctions& currentDisjunction = allSubResults.back();
                        for( auto consConj = subResult.begin(); consConj != subResult.end(); ++consConj )
                        {
                            currentDisjunction.push_back( ConditionList() );
                            ConditionList& currentConjunction = currentDisjunction.back();
                            for( auto cons = consConj->begin(); cons != consConj->end(); ++cons )
                            {
                                currentConjunction.push_back( new vs::Condition( *cons, mpConditionIdAllocator->getId(), _currentState->treeDepth() ) );
                                currentConjunction.back()->pOriginalConditions()->insert( *cond );
                            }
                        }
                    }
                }
            }
        }
        bool cleanResultsOfThisMethod = false;
        if( anySubstitutionFailed )
        {
            _currentState->rFather().addConflicts( _currentState->pSubstitution(), conflictSet );
            _currentState->rInconsistent() = true;
            while( !_currentState->rConflictSets().empty() )
            {
                const Substitution* sub = _currentState->rConflictSets().begin()->first;
                _currentState->rConflictSets().erase( _currentState->rConflictSets().begin() );
                if( sub != NULL && sub->type() == Substitution::Type::INVALID )
                {
                    delete sub;
                }
            }
            while( !_currentState->children().empty() )
            {
                State* toDelete = _currentState->rChildren().back();
                removeStatesFromRanking( *toDelete );
                _currentState->rChildren().pop_back();
                _currentState->resetInfinityChild( toDelete );
                delete toDelete;  // DELETE STATE
            }
            _currentState->updateIntTestCandidates();
            while( !_currentState->conditions().empty() )
            {
                const vs::Condition* pCond = _currentState->rConditions().back();
                _currentState->rConditions().pop_back();
                #ifdef SMTRAT_VS_VARIABLEBOUNDS
                _currentState->rVariableBounds().removeBound( pCond->pConstraint(), pCond );
                #endif
                mpConditionIdAllocator->free( pCond->getId() );
                delete pCond;
                pCond = NULL;
            }
            cleanResultsOfThisMethod = true;
        }
        else
        {
            if( !_currentState->isInconsistent() )
            {
                if( allSubstitutionsApplied )
                {
                    removeStatesFromRanking( *_currentState );
                    allSubResults.push_back( DisjunctionOfConditionConjunctions() );
                    allSubResults.back().push_back( oldConditions );
                    _currentState->addSubstitutionResults( allSubResults );
                    #ifdef VS_MODULE_VERBOSE_INTEGERS
                    _currentState->printSubstitutionResults( string( _currentState->treeDepth()*3, ' '), cout );
                    #endif
                    addStatesToRanking( _currentState );
                }
                else
                {
                    removeStatesFromRanking( _currentState->rFather() );
                    _currentState->resetConflictSets();
                    while( !_currentState->children().empty() )
                    {
                        State* toDelete = _currentState->rChildren().back();
                        _currentState->rChildren().pop_back();
                        _currentState->resetInfinityChild( toDelete );
                        delete toDelete;  // DELETE STATE
                    }
                    _currentState->updateIntTestCandidates();
                    while( !_currentState->conditions().empty() )
                    {
                        const vs::Condition* pCond = _currentState->rConditions().back();
                        _currentState->rConditions().pop_back();
                        #ifdef SMTRAT_VS_VARIABLEBOUNDS
                        _currentState->rVariableBounds().removeBound( pCond->pConstraint(), pCond );
                        #endif
                        mpConditionIdAllocator->free( pCond->getId() );
                        delete pCond;
                        pCond = NULL;
                    }
                    _currentState->rMarkedAsDeleted() = true;
                    _currentState->rFather().rCannotBeSolved() = true;
                    addStatesToRanking( _currentState->pFather() );
                    cleanResultsOfThisMethod = true;
                }
            }
            #ifdef VS_DEBUG
            _currentState->print( "   ", cout );
            #endif
        }
        if( cleanResultsOfThisMethod )
        {
            while( !oldConditions.empty() )
            {
                const vs::Condition* rpCond = oldConditions.back();
                oldConditions.pop_back();
                mpConditionIdAllocator->free( rpCond->getId() );
                delete rpCond;
                rpCond = NULL;
            }
            while( !allSubResults.empty() )
            {
                while( !allSubResults.back().empty() )
                {
                    while( !allSubResults.back().back().empty() )
                    {
                        const vs::Condition* rpCond = allSubResults.back().back().back();
                        allSubResults.back().back().pop_back();
                        mpConditionIdAllocator->free( rpCond->getId() );
                        delete rpCond;
                        rpCond = NULL;
                    }
                    allSubResults.back().pop_back();
                }
                allSubResults.pop_back();
            }
        }
        return !anySubstitutionFailed;
    }

    template<class Settings>
    void VSModule<Settings>::propagateNewConditions( State* _currentState )
    {
        
        removeStatesFromRanking( *_currentState );
        // Collect the recently added conditions and mark them as not recently added.
        bool deleteExistingTestCandidates = false;
        ConditionList recentlyAddedConditions;
        for( auto cond = _currentState->rConditions().begin(); cond != _currentState->conditions().end(); ++cond )
        {
            if( (**cond).recentlyAdded() )
            {
                (**cond).rRecentlyAdded() = false;
                recentlyAddedConditions.push_back( *cond );
                if( _currentState->pOriginalCondition() == NULL )
                {
                    bool onlyTestCandidateToConsider = false;
                    if( _currentState->index() != carl::Variable::NO_VARIABLE ) // TODO: Maybe only if the degree is not to high
                        onlyTestCandidateToConsider = (**cond).constraint().hasFinitelyManySolutionsIn( _currentState->index() );
                    if( onlyTestCandidateToConsider )
                        deleteExistingTestCandidates = true;
                }
            }
        }
        addStateToRanking( _currentState );
        if( !_currentState->children().empty() )
        {
            if( deleteExistingTestCandidates || _currentState->initIndex( mAllVariables, Settings::prefer_equation_over_all ) )
            {
                _currentState->initConditionFlags();
                // If the recently added conditions make another variable being the best to eliminate next delete all children.
                _currentState->resetConflictSets();
                while( !_currentState->children().empty() )
                {
                    State* toDelete = _currentState->rChildren().back();
                    _currentState->rChildren().pop_back();
                    _currentState->resetInfinityChild( toDelete );
                    delete toDelete;  // DELETE STATE
                }
                _currentState->updateIntTestCandidates();
            }
            else
            {
                bool newTestCandidatesGenerated = false;
                if( _currentState->pOriginalCondition() == NULL )
                {
                    /*
                     * Check if there are new conditions in this state, which would have been better choices
                     * to generate test candidates than those conditions of the already generated test
                     * candidates. If so, generate the test candidates of the better conditions.
                     */
                    for( auto cond = recentlyAddedConditions.begin(); cond != recentlyAddedConditions.end(); ++cond )
                    {
                        if( _currentState->index() != carl::Variable::NO_VARIABLE && (**cond).constraint().hasVariable( _currentState->index() ) )
                        {
                            bool worseConditionFound = false;
                            auto child = _currentState->rChildren().begin();
                            while( !worseConditionFound && child != _currentState->children().end() )
                            {
                                if( (**child).substitution().type() != Substitution::MINUS_INFINITY || (**child).substitution().type() != Substitution::PLUS_INFINITY)
                                {
                                    auto oCond = (**child).rSubstitution().rOriginalConditions().begin();
                                    while( !worseConditionFound && oCond != (**child).substitution().originalConditions().end() )
                                    {
                                        if( (**cond).valuate( _currentState->index(), mAllVariables.size(), Settings::prefer_equation_over_all ) > (**oCond).valuate( _currentState->index(), mAllVariables.size(), Settings::prefer_equation_over_all ) )
                                        {
                                            newTestCandidatesGenerated = true;
                                            #ifdef VS_DEBUG
                                            cout << "*** Eliminate " << _currentState->index() << " in ";
                                            cout << (**cond).constraint().toString( 0, true, true );
                                            cout << " creates:" << endl;
                                            #endif
                                            eliminate( _currentState, _currentState->index(), *cond );
                                            #ifdef VS_DEBUG
                                            cout << "*** Eliminate ready." << endl;
                                            #endif
                                            worseConditionFound = true;
                                            break;
                                        }
                                        ++oCond;
                                    }
                                }
                                ++child;
                            }
                        }
                    }
                }
                // Otherwise, add the recently added conditions to each child of the considered state to
                // which a substitution must be or already has been applied.
                for( auto child = _currentState->rChildren().begin(); child != _currentState->children().end(); ++child )
                {
                    if( (**child).type() != State::SUBSTITUTION_TO_APPLY || (**child).isInconsistent() )
                    {
                        // Apply substitution to new conditions and add the result to the substitution result vector.
                        if( !substituteAll( *child, recentlyAddedConditions ) )
                        {
                            // Delete the currently considered state.
                            assert( (*child)->rInconsistent() );
                            assert( (**child).conflictSets().empty() );
                            removeStateFromRanking( **child );
                        }
                        else if( (**child).isInconsistent() && !(**child).subResultsSimplified() && !(**child).conflictSets().empty() )
                        {
                            addStateToRanking( *child );
                        }
                    }
                    else
                    {
                        if( newTestCandidatesGenerated )
                        {
                            if( !(**child).children().empty() )
                            {
                                (**child).rHasChildrenToInsert() = true;
                            }
                        }
                        else
                        {
                            addStatesToRanking( *child );
                        }
                    }
                }
            }
        }
        _currentState->rHasRecentlyAddedConditions() = false;
    }
    
    template<class Settings>
    void VSModule<Settings>::addStateToRanking( State* _state )
    {
        if( !_state->markedAsDeleted() && !(_state->isInconsistent() && _state->conflictSets().empty() && _state->conditionsSimplified()) )
        {
            if( _state->id() != 0 )
            {
                size_t id = _state->id();
                removeStateFromRanking( *_state );
                _state->rID()= id;
            }
            else
            {
                increaseIDCounter();
                _state->rID() = mIDCounter;
            }
            _state->updateValuation();
            UnsignedTriple key = UnsignedTriple( _state->valuation(), std::pair< size_t, size_t> ( _state->id(), _state->backendCallValuation() ) );
            if( (mRanking.insert( ValStatePair( key, _state ) )).second == false )
            {
                cout << "Warning: Could not insert. Entry already exists.";
                cout << endl;
            }
        }
    }

    template<class Settings>
    void VSModule<Settings>::addStatesToRanking( State* _state )
    {
        addStateToRanking( _state );
        if( _state->conditionsSimplified() && _state->subResultsSimplified() && !_state->takeSubResultCombAgain() && !_state->hasRecentlyAddedConditions() )
            for( auto dt = (*_state).rChildren().begin(); dt != (*_state).children().end(); ++dt )
                addStatesToRanking( *dt );
    }

    template<class Settings>
    void VSModule<Settings>::insertTooHighDegreeStatesInRanking( State* _state )
    {
        if( _state->cannotBeSolved() )
            addStateToRanking( _state );
        else
            for( auto dt = (*_state).rChildren().begin(); dt != (*_state).children().end(); ++dt )
                insertTooHighDegreeStatesInRanking( *dt );
    }

    template<class Settings>
    bool VSModule<Settings>::removeStateFromRanking( State& _state )
    {
        UnsignedTriple key = UnsignedTriple( _state.valuation(), std::pair< unsigned, unsigned> ( _state.id(), _state.backendCallValuation() ) );
        auto valDTPair = mRanking.find( key );
        if( valDTPair != mRanking.end() )
        {
            mRanking.erase( valDTPair );
            _state.rID() = 0;
            return true;
        }
        else
            return false;
    }

    template<class Settings>
    void VSModule<Settings>::removeStatesFromRanking( State& _state )
    {
        removeStateFromRanking( _state );
        for( auto dt = _state.rChildren().begin(); dt != _state.children().end(); ++dt )
            removeStatesFromRanking( **dt );
    }
    
    template<class Settings>
    bool VSModule<Settings>::checkRanking() const
    {
        for( auto valDTPair = mRanking.begin(); valDTPair != mRanking.end(); ++valDTPair )
        {
            if( !mpStateTree->containsState( valDTPair->second ) )
                return false;
        }
        return true;
    }

    template<class Settings>
    FormulasT VSModule<Settings>::getReasons( const carl::PointerSet<vs::Condition>& _conditions ) const
    {
        FormulasT result;
        if( _conditions.empty() ) return result;
        // Get the original conditions of the root of the root state leading to the given set of conditions.
        carl::PointerSet<vs::Condition> conds = _conditions;
        carl::PointerSet<vs::Condition> oConds;
        while( !(*conds.begin())->originalConditions().empty() )
        {
            for( auto cond = conds.begin(); cond != conds.end(); ++cond )
            {
                assert( !(*cond)->originalConditions().empty() );
                oConds.insert( (*cond)->originalConditions().begin(), (*cond)->originalConditions().end() );
            }
            conds.clear();
            conds.swap( oConds );
        }
        // Get the sub-formulas in the received formula corresponding to these original conditions.
        for( auto oCond = conds.begin(); oCond != conds.end(); ++oCond )
        {
            assert( (*oCond)->pConstraint() != NULL );
            assert( (*oCond)->originalConditions().empty() );
            auto receivedConstraint = rReceivedFormula().begin();
            while( receivedConstraint != rReceivedFormula().end() )
            {
                if( receivedConstraint->formula().getType() == carl::FormulaType::CONSTRAINT )
                {
                    if( (**oCond).constraint() == receivedConstraint->formula().constraint() )
                        break;
                }
                ++receivedConstraint;
            }
            assert( receivedConstraint != rReceivedFormula().end() );
            result.insert( receivedConstraint->formula() );
        }
        return result;
    }
    
    template<class Settings>
    void VSModule<Settings>::updateInfeasibleSubset( bool _includeInconsistentTestCandidates )
    {
        if( !Settings::infeasible_subset_generation )
        {
            // Set the infeasible subset to the set of all received constraints.
            mInfeasibleSubsets.push_back( FormulasT() );
            for( auto cons = rReceivedFormula().begin(); cons != rReceivedFormula().end(); ++cons )
                mInfeasibleSubsets.back().insert( cons->formula() );
            return;
        }
        // Determine the minimum covering sets of the conflict sets, i.e. the infeasible subsets of the root.
        ConditionSetSet minCoverSets;
        ConditionSetSetSet confSets;
        State::ConflictSets::iterator nullConfSet = mpStateTree->rConflictSets().find( NULL );
        if( nullConfSet != mpStateTree->rConflictSets().end() && !_includeInconsistentTestCandidates )
            confSets.insert( nullConfSet->second.begin(), nullConfSet->second.end() );
        else
            for( auto confSet = mpStateTree->rConflictSets().begin(); confSet != mpStateTree->rConflictSets().end(); ++confSet )
                confSets.insert( confSet->second.begin(), confSet->second.end() );
        allMinimumCoveringSets( confSets, minCoverSets );
        assert( !minCoverSets.empty() );
        // Change the globally stored infeasible subset to the smaller one.
        mInfeasibleSubsets.clear();
        for( auto minCoverSet = minCoverSets.begin(); minCoverSet != minCoverSets.end(); ++minCoverSet )
        {
            assert( !minCoverSet->empty() );
            mInfeasibleSubsets.push_back( getReasons( *minCoverSet ) );
        }
        assert( !mInfeasibleSubsets.empty() );
        assert( !mInfeasibleSubsets.back().empty() );
    }
    
    template<class Settings>
    EvalRationalMap VSModule<Settings>::getIntervalAssignment( const State* _state ) const
    {
        EvalRationalMap varSolutions;
        // The variables have 0 as default assignment, which is going to be overwritten
        // if the variable has been eliminated by virtual substitution. Note, that it is
        // possible that variables are eliminated as a side effect of the elimination of 
        // a different variable, which means that we can choose the assignment of that 
        // variable arbitrarily.
        carl::Variables vars;
        _state->father().variables( vars );
        vars.erase( _state->substitution().variable() );
        while( !vars.empty() )
        {
            varSolutions[*vars.begin()] = ZERO_RATIONAL;
            vars.erase( vars.begin() );
        }
        // Find all assignments of the variables occurring in the conditions
        // of the state's father except of the variable being the index currentState.
        const State* successorState = mRanking.begin()->second;
        while( successorState != _state )
        {
            assert( !successorState->isRoot() );
            assert( successorState->substitution().variable().getType() == carl::VariableType::VT_INT );
            assert( successorState->substitution().type() == Substitution::NORMAL );
            Rational subTermEvaluated;
            successorState->substitution().term().evaluate( subTermEvaluated, varSolutions );
            varSolutions[successorState->substitution().variable()] = subTermEvaluated;
            successorState = successorState->pFather();
        }
        return varSolutions;
    }
    
    template<class Settings>
    bool VSModule<Settings>::sideConditionsSatisfied( const vs::Substitution& _substitution, const EvalRationalMap& _assignment )
    {
        for( const ConstraintT* sideC : _substitution.sideCondition() )
        {
            unsigned sideCisConsistent = sideC->satisfiedBy( _assignment );
            assert( sideCisConsistent != 2 );
            if( sideCisConsistent == 0 )
            {
                return false;
            }
        }
        return true;
    }
    
    template<class Settings>
    bool VSModule<Settings>::solutionInDomain()
    {
//        std::cout << __func__ << std::endl;
        assert( solverState() != False );
        if( !mRanking.empty() )
        {
            std::vector<carl::Variable> varOrder;
            State* currentState = mRanking.begin()->second;
            while( !currentState->isRoot() )
            {
//                currentState->printAlone();
                if( currentState->substitution().variable().getType() == carl::VariableType::VT_INT )
                {
                    if( Settings::branch_and_bound && (currentState->substitution().type() == Substitution::MINUS_INFINITY || currentState->substitution().type() == Substitution::PLUS_INFINITY ) )
                    {
                        Rational nextIntTCinRange;
                        if( currentState->getNextIntTestCandidate( nextIntTCinRange, Settings::int_max_range ) )
                        {
                            branchAt( Poly( currentState->substitution().variable() ), nextIntTCinRange, getReasons( currentState->substitution().originalConditions() ) );
                        }
                        else
                        {
                            removeStatesFromRanking( *currentState );
                            currentState->rCannotBeSolved() = true;
                            addStateToRanking( currentState );
                        }
                        return false;
                    }
                    else
                    {
                        assert( currentState->substitution().type() != Substitution::PLUS_EPSILON );
                        EvalRationalMap varSolutions = getIntervalAssignment( currentState );
                        if( Settings::branch_and_bound )
                        {
                            EvalRationalMap partialVarSolutions;
                            const Poly& substitutionPoly = (*currentState->substitution().originalConditions().begin())->constraint().lhs();
                            for( auto var = varOrder.rbegin(); var != varOrder.rend(); ++var )
                            {
                                assert( varSolutions.find( *var ) != varSolutions.end() );
//                                std::cout << "check if origin  " << substitutionPoly << "  has integer zeros under the assignment" << std::endl;
                                partialVarSolutions[*var] = varSolutions[*var];
//                                for( const auto& pvs : partialVarSolutions )
//                                    std::cout << "      " << pvs.first << " -> " << pvs.second << std::endl;
//                                std::cout << "check first if side conditions of test candidate are fulfilled:" << std::endl;
                                Poly subPolyPartiallySubstituted = substitutionPoly.substitute( partialVarSolutions );
//                                std::cout << "   results in " << subPolyPartiallySubstituted << std::endl;
                                auto term = subPolyPartiallySubstituted.rbegin();
                                if( term != subPolyPartiallySubstituted.rend() )
                                {
//                                    std::cout << "   check if the gcd of the coefficients divides the constant part:" << std::endl;
                                    assert( !term->isConstant() && carl::isInteger( term->coeff() ) );
                                    Rational g = carl::abs( term->coeff() );
                                    ++term;
                                    for( ; term != subPolyPartiallySubstituted.rend(); ++term )
                                    {
                                        if( !term->isConstant() )
                                        {
                                            assert( carl::isInteger( term->coeff() ) );
                                            g = carl::gcd( carl::getNum( g ), carl::getNum( carl::abs( term->coeff() ) ) );
                                        }
                                    }
                                    assert( g > ZERO_RATIONAL );
//                                    std::cout << "   " << carl::getNum( subPolyPartiallySubstituted.constantPart() ) << " mod " << carl::getNum( g ) << " == " << carl::mod( carl::getNum( subPolyPartiallySubstituted.constantPart() ), carl::getNum( g ) ) << std::endl; 
                                    if( carl::mod( carl::getNum( subPolyPartiallySubstituted.constantPart() ), carl::getNum( g ) ) != 0 )
                                    {
                                        Poly branchEx = ((subPolyPartiallySubstituted - subPolyPartiallySubstituted.constantPart()) * (1 / g));
                                        Rational branchValue = subPolyPartiallySubstituted.constantPart() * (1 / g);
                                        branchAt( branchEx, branchValue, getReasons( currentState->substitution().originalConditions() ) );
                                        return false;
                                    }
                                }
                            }
                        }
                        // Insert the (integer!) assignments of the other variables.
                        const SqrtEx& subTerm = currentState->substitution().term();
                        assert( sideConditionsSatisfied( currentState->substitution(), varSolutions ) );
                        Rational evaluatedSubTerm;
//                        std::cout << "replace variable in  " << subTerm << "  by" << std::endl;
//                        for( const auto& vs : varSolutions )
//                            std::cout << "   " << vs.first << " -> " << vs.second << std::endl;
                        bool assIsInteger = subTerm.evaluate( evaluatedSubTerm, varSolutions, -1 );
//                        std::cout << "results in " << evaluatedSubTerm << std::endl;
                        assIsInteger &= carl::isInteger( evaluatedSubTerm );
                        if( !assIsInteger )
                        {
                            if( Settings::branch_and_bound )
                            {
                                branchAt( Poly( currentState->substitution().variable() ), evaluatedSubTerm, getReasons( currentState->substitution().originalConditions() ) );
                            }
                            else
                            {
                                // Add test candidate to father, being the next greater integer than the found non-integer assignment of the
                                // substituted variable of the current state.
                                State* toRemove = mRanking.begin()->second;
                                const Substitution& currSub = currentState->substitution();
                                SqrtEx t = SqrtEx( Poly( cln::floor1( evaluatedSubTerm ) + 1 ) );
                                Substitution newSub = Substitution( currSub.variable(), t, Substitution::Type::NORMAL, currSub.originalConditions() );
                                std::vector<State*> addedChildren = currentState->rFather().addChild( newSub );
                                if( !addedChildren.empty() )
                                {
                                    // Add its valuation to the current ranking.
                                    while( !addedChildren.empty() )
                                    {
                                        addStatesToRanking( addedChildren.back() );
                                        addedChildren.pop_back();
                                    }
                                    #ifdef VS_DEBUG
                                    currentState->rFather().rChildren().back()->print( "   ", cout );
                                    #endif
                                }
                                removeStatesFromRanking( *toRemove );
                                toRemove->rFather().rChildren().remove( toRemove );
                                delete toRemove;  // DELETE STATE
                            }
                            return false;
                        }
                    }
                }
                varOrder.push_back( currentState->substitution().variable() );
                currentState = currentState->pFather();
            }
        }
        return true;
    }

    template<class Settings>
    void VSModule<Settings>::allMinimumCoveringSets( const ConditionSetSetSet& _conflictSets, ConditionSetSet& _minCovSets )
    {
        if( !_conflictSets.empty() )
        {
            // First we construct all possible combinations of combining all single sets of each set of sets.
            // Store for each set an iterator.
            std::vector<ConditionSetSet::iterator> conditionSetSetIters = std::vector<ConditionSetSet::iterator>();
            for( auto conflictSet = _conflictSets.begin(); conflictSet != _conflictSets.end(); ++conflictSet )
            {
                conditionSetSetIters.push_back( (*conflictSet).begin() );
                // Assure, that the set is not empty.
                assert( conditionSetSetIters.back() != (*conflictSet).end() );
            }
            ConditionSetSetSet::iterator conflictSet;
            std::vector<ConditionSetSet::iterator>::iterator conditionSet;
            // Find all covering sets by forming the union of all combinations.
            bool lastCombinationReached = false;
            while( !lastCombinationReached )
            {
                // Create a new combination of vectors.
                carl::PointerSet<vs::Condition> coveringSet;
                bool previousIteratorIncreased = false;
                // For each set of sets in the vector of sets of sets, choose a set in it. We combine
                // these sets by forming their union and store it as a covering set.
                conditionSet = conditionSetSetIters.begin();
                conflictSet  = _conflictSets.begin();
                while( conditionSet != conditionSetSetIters.end() )
                {
                    if( (*conflictSet).empty() )
                    {
                        conflictSet++;
                        conditionSet++;
                    }
                    else
                    {
                        coveringSet.insert( (**conditionSet).begin(), (**conditionSet).end() );
                        // Set the iterator.
                        if( !previousIteratorIncreased )
                        {
                            (*conditionSet)++;
                            if( *conditionSet != (*conflictSet).end() )
                                previousIteratorIncreased = true;
                            else
                                *conditionSet = (*conflictSet).begin();
                        }
                        conflictSet++;
                        conditionSet++;
                        if( !previousIteratorIncreased && conditionSet == conditionSetSetIters.end() )
                            lastCombinationReached = true;
                    }
                }
                _minCovSets.insert( coveringSet );
            }
            /*
             * Delete all covering sets, which are not minimal. We benefit of the set of sets property,
             * which sorts its sets according to the elements they contain. If a set M is a subset of
             * another set M', the position of M in the set of sets is before M'.
             */
            auto minCoverSet = _minCovSets.begin();
            auto coverSet    = _minCovSets.begin();
            coverSet++;
            while( coverSet != _minCovSets.end() )
            {
                auto cond1 = (*minCoverSet).begin();
                auto cond2 = (*coverSet).begin();
                while( cond1 != (*minCoverSet).end() && cond2 != (*coverSet).end() )
                {
                    if( *cond1 != *cond2 )
                        break;
                    cond1++;
                    cond2++;
                }
                if( cond1 == (*minCoverSet).end() )
                    _minCovSets.erase( coverSet++ );
                else
                {
                    minCoverSet = coverSet;
                    coverSet++;
                }
            }
        }
    }

    template<class Settings>
    bool VSModule<Settings>::adaptPassedFormula( const State& _state, FormulaConditionMap& _formulaCondMap )
    {
        if( _state.conditions().empty() ) return false;
        bool changedPassedFormula = false;
        // Collect the constraints to check.
        carl::PointerMap<ConstraintT,const vs::Condition*> constraintsToCheck;
        for( auto cond = _state.conditions().begin(); cond != _state.conditions().end(); ++cond )
        {
            // Optimization: If the zeros of the polynomial in a weak inequality have already been checked pass the strict version.
            if( _state.allTestCandidatesInvalidated( *cond ) )
            {
                const ConstraintT* constraint = (*cond)->pConstraint();
                switch( constraint->relation() )
                {
                    case carl::Relation::GEQ:
                    {
                        const ConstraintT* strictVersion = carl::newConstraint<Poly>( constraint->lhs(), carl::Relation::GREATER );
                        constraintsToCheck.insert( std::pair< const ConstraintT*, const vs::Condition*>( strictVersion, *cond ) );
                        break;
                    }
                    case carl::Relation::LEQ:
                    {
                        const ConstraintT* strictVersion = carl::newConstraint<Poly>( constraint->lhs(), carl::Relation::LESS );
                        constraintsToCheck.insert( std::pair< const ConstraintT*, const vs::Condition*>( strictVersion, *cond ) );
                        break;
                    }
                    default:
                    {
                        constraintsToCheck.insert( std::pair< const ConstraintT*, const vs::Condition*>( constraint, *cond ) );
                    }
                }
            }
            else
                constraintsToCheck.insert( std::pair< const ConstraintT*, const vs::Condition*>( (*cond)->pConstraint(), *cond ) );
        }
        /*
         * Remove the constraints from the constraints to check, which are already in the passed formula
         * and remove the sub formulas (constraints) in the passed formula, which do not occur in the
         * constraints to add.
         */
        auto subformula = passedFormulaBegin();
        while( subformula != rPassedFormula().end() )
        {
            auto iter = constraintsToCheck.find( subformula->formula().pConstraint() );
            if( iter != constraintsToCheck.end() )
            {
                _formulaCondMap[subformula->formula()] = iter->second;
                constraintsToCheck.erase( iter );
                ++subformula;
            }
            else
            {
                subformula = eraseSubformulaFromPassedFormula( subformula );
                changedPassedFormula = true;
            }
        }
        // Add the the remaining constraints to add to the passed formula.
        for( auto iter = constraintsToCheck.begin(); iter != constraintsToCheck.end(); ++iter )
        {
            changedPassedFormula = true;
            std::vector<FormulasT> origins;
            // @todo store formula and do not generate a new formula every time
            FormulaT formula = FormulaT( iter->first );
            _formulaCondMap[formula] = iter->second;
            addConstraintToInform( formula );
            addSubformulaToPassedFormula( formula, std::move( origins ) );
        }
        return changedPassedFormula;
    }

    template<class Settings>
    Answer VSModule<Settings>::runBackendSolvers( State* _state )
    {
        // Run the backends on the constraint of the state.
        FormulaConditionMap formulaToConditions = FormulaConditionMap();
        adaptPassedFormula( *_state, formulaToConditions );
        Answer result = runBackends();
        #ifdef VS_DEBUG
        cout << "Ask backend      : ";
        printPassedFormula();
        cout << endl;
        cout << "Answer           : " << ( result == True ? "True" : ( result == False ? "False" : "Unknown" ) ) << endl;
        #endif
        switch( result )
        {
            case True:
            {
                return True;
            }
            case False:
            {
                /*
                * Get the conflict sets formed by the infeasible subsets in the backend.
                */
                ConditionSetSet conflictSet = ConditionSetSet();
                std::vector<Module*>::const_iterator backend = usedBackends().begin();
                while( backend != usedBackends().end() )
                {
                    if( !(*backend)->infeasibleSubsets().empty() )
                    {
                        for( auto infsubset = (*backend)->infeasibleSubsets().begin(); infsubset != (*backend)->infeasibleSubsets().end(); ++infsubset )
                        {
                            carl::PointerSet<vs::Condition> conflict;
                            #ifdef VS_DEBUG
                            cout << "Infeasible Subset: {";
                            #endif
                            for( auto subformula = infsubset->begin(); subformula != infsubset->end(); ++subformula )
                            {
                                #ifdef VS_DEBUG
                                cout << "  " << *subformula;
                                #endif
                                auto fcPair = formulaToConditions.find( *subformula );
                                assert( fcPair != formulaToConditions.end() );
                                conflict.insert( fcPair->second );
                            }
                            #ifdef VS_DEBUG
                            cout << "  }" << endl;
                            #endif
                            #ifdef SMTRAT_DEVOPTION_Validation
                            if( validationSettings->logTCalls() )
                            {
                                carl::PointerSet<smtrat::ConstraintT> constraints;
                                for( auto cond = conflict.begin(); cond != conflict.end(); ++cond )
                                    constraints.insert( (**cond).pConstraint() );
                                smtrat::Module::addAssumptionToCheck( constraints, false, moduleName( (*backend)->type() ) + "_infeasible_subset" );
                            }
                            #endif
                            assert( conflict.size() == infsubset->size() );
                            assert( !conflict.empty() );
                            conflictSet.insert( conflict );
                        }
                        break;
                    }
                }
                assert( !conflictSet.empty() );
                _state->addConflictSet( NULL, conflictSet );
                removeStatesFromRanking( *_state );

                #ifdef VS_LOG_INTERMEDIATE_STEPS
                logConditions( *_state, false, "Intermediate_conflict_of_VSModule" );
                #endif
                // If the considered state is the root, update the found infeasible subset as infeasible subset.
                if( _state->isRoot() )
                    updateInfeasibleSubset();
                // If the considered state is not the root, pass the infeasible subset to the father.
                else
                {
                    removeStatesFromRanking( *_state );
                    _state->passConflictToFather( Settings::check_conflict_for_side_conditions );
                    removeStateFromRanking( _state->rFather() );
                    addStateToRanking( _state->pFather() );
                }
                return False;
            }
            case Unknown:
            {
                return Unknown;
            }
            default:
            {
                cerr << "Unknown answer type!" << endl;
                assert( false );
                return Unknown;
            }
        }
    }

    /**
     * Checks the correctness of the symbolic assignment given by the path from the root
     * state to the satisfying state.
     */
    template<class Settings>
    void VSModule<Settings>::checkAnswer() const
    {
        if( !mRanking.empty() )
        {
            const State* currentState = mRanking.begin()->second;
            while( !(*currentState).isRoot() )
            {
                logConditions( *currentState, true, "Intermediate_result_of_VSModule" );
                currentState = currentState->pFather();
            }
        }
    }

    template<class Settings>
    void VSModule<Settings>::logConditions( const State& _state, bool _assumption, const string& _description ) const
    {
        if( !_state.conditions().empty() )
        {
            carl::PointerSet<smtrat::ConstraintT> constraints;
            for( auto cond = _state.conditions().begin(); cond != _state.conditions().end(); ++cond )
                constraints.insert( (**cond).pConstraint() );
            smtrat::Module::addAssumptionToCheck( constraints, _assumption, _description );
        }
    }

    template<class Settings>
    void VSModule<Settings>::printAll( const string& _init, ostream& _out ) const
    {
        _out << _init << " Current solver status, where the constraints" << endl;
        printFormulaConditionMap( _init, _out );
        _out << _init << " have been added:" << endl;
        _out << _init << " mInconsistentConstraintAdded: " << mInconsistentConstraintAdded << endl;
        _out << _init << " mIDCounter: " << mIDCounter << endl;
        _out << _init << " Current ranking:" << endl;
        printRanking( _init, cout );
        _out << _init << " State tree:" << endl;
        mpStateTree->print( _init + "   ", _out );
    }

    template<class Settings>
    void VSModule<Settings>::printFormulaConditionMap( const string& _init, ostream& _out ) const
    {
        for( auto cond = mFormulaConditionMap.begin(); cond != mFormulaConditionMap.end(); ++cond )
        {
            _out << _init << "    ";
            _out << cond->first.toString( false, 0, "", true, true, true );
            _out << " <-> ";
            cond->second->print( _out );
            _out << endl;
        }
    }

    template<class Settings>
    void VSModule<Settings>::printRanking( const string& _init, ostream& _out ) const
    {
        for( auto valDTPair = mRanking.begin(); valDTPair != mRanking.end(); ++valDTPair )
            (*(*valDTPair).second).printAlone( _init + "   ", _out );
    }

    template<class Settings>
    void VSModule<Settings>::printAnswer( const string& _init, ostream& _out ) const
    {
        _out << _init << " Answer:" << endl;
        if( mRanking.empty() )
            _out << _init << "        False." << endl;
        else
        {
            _out << _init << "        True:" << endl;
            const State* currentState = mRanking.begin()->second;
            while( !(*currentState).isRoot() )
            {
                _out << _init << "           " << (*currentState).substitution().toString( true ) << endl;
                currentState = (*currentState).pFather();
            }
        }
        _out << endl;
    }
}    // end namespace smtrat
