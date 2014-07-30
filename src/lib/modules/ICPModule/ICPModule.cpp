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
/*
 * @file   ICPModule.cpp
 * @author Stefan Schupp <stefan.schupp@rwth-aachen.de>
 *
 * Created on October 16, 2012, 1:07 PM
 */

#include <map>
#include "ICPModule.h"
#include "assert.h"

using namespace std;
using namespace carl;

//#define ICP_MODULE_DEBUG_0
//#define ICP_MODULE_DEBUG_1
#define ICP_CONSIDER_WIDTH
//#define ICP_SIMPLE_VALIDATION
#define ICP_PROLONG_CONTRACTION

namespace smtrat
{
    /**
     * Constructor
     */
    ICPModule::ICPModule( ModuleType _type, const ModuleInput* _formula, RuntimeSettings* , Conditionals& _conditionals, Manager* const _manager ):
        Module( _type, _formula, _conditionals, _manager ),
        mActiveNonlinearConstraints(),
        mActiveLinearConstraints(),
        mLinearConstraints(),
        mNonlinearConstraints(),
        mVariables(),
        mIntervals(),
        mIcpRelevantCandidates(),
        mLinearizations(),
        mDeLinearizations(),
        mVariableLinearizations(),
        mSubstitutions(),
        //#ifdef BOXMANAGEMENT
        mHistoryRoot(new icp::HistoryNode(mIntervals,1)),
        mHistoryActual(NULL),
        //#endif
        mValidationFormula(new ModuleInput()),
        mLRAFoundAnswer( vector< std::atomic_bool* >( 1, new std::atomic_bool( false ) ) ),
        mLraRuntimeSettings(new RuntimeSettings),
        mLRA(MT_LRAModule, mValidationFormula, mLraRuntimeSettings, mLRAFoundAnswer),
        mReceivedConstraints(),
        mCenterConstraints(),
        mCreatedDeductions(),
        mLastCandidate(NULL),
        #ifndef BOXMANAGEMENT
        mBoxStorage(),
        #endif
        mIsIcpInitialized(false),
        mCurrentId(1),
        mIsBackendCalled(false),
        mTargetDiameter(0.01),
        mContractionThreshold(0.001),
        mCountBackendCalls(0)
    {
        #ifdef ICP_BOXLOG
        icpLog.open ("icpLog.txt", ios::out | ios::trunc );
        #endif
    }

    /**
     * Destructor:
     */
    ICPModule::~ICPModule()
    {
        mLRAFoundAnswer.clear();
        delete mLraRuntimeSettings;
        #ifdef BOXMANAGEMENT
        delete mHistoryRoot;
        #endif
        delete mValidationFormula;
        mLRAFoundAnswer.clear();
        
        for(auto variableIt = mVariables.begin(); variableIt != mVariables.end(); ++variableIt)
            delete (*variableIt).second;
        
        mVariables.clear();
        #ifdef ICP_BOXLOG
        if ( icpLog.is_open() )
        {
            icpLog.close();
        }
        #endif
    }

    bool ICPModule::inform( const Constraint* const _constraint )
    {
        #ifdef ICP_MODULE_DEBUG_0
        cout << "[ICP] inform: " << (*_constraint) << " (id: " << _constraint->id() << ")" << endl;
        #endif  
        // do not inform about boundary constraints - this leads to confusion
        if ( !_constraint->isBound() )
            Module::inform( _constraint );
        
        unsigned constraintConsistency = _constraint->isConsistent();
        
        if( constraintConsistency == 2 )
        {
            const Formula* constraintAsFormula = newFormula( _constraint );  // TODO (From Florian): Can we omit the construction here?
            addConstraint( constraintAsFormula );
        }
        return constraintConsistency != 0;
    }

    bool ICPModule::assertSubformula( ModuleInput::const_iterator _formula )
    {
        switch( (*_formula)->getType() )
        {
            case FFALSE:
            {
                PointerSet<Formula> infSubSet;
                infSubSet.insert( *_formula );
                mInfeasibleSubsets.push_back( infSubSet );
                mFoundSolution.clear();
                return false;
            }
            case TTRUE:
            {
                return true;
            }
            case CONSTRAINT:
            {
                // Avoid constraints to be added twice to the icp module internals, as this provokes undefined behavior
                auto rc = mReceivedConstraints.find( (*_formula)->pConstraint() );
                if( rc != mReceivedConstraints.end() )
                {
                    ++(rc->second);
                    return true;
                }
                else
                {
                    mReceivedConstraints.insert( std::pair<const Constraint*, unsigned>( (*_formula)->pConstraint(), 1 ) );
                }
                const Constraint& constr = (*_formula)->constraint();
                // create and initialize slackvariables
                if( constr.satisfiedBy( mFoundSolution ) != 1 )
                {
                    mFoundSolution.clear();
                }
                if( !mIsIcpInitialized )
                {
                    // catch deductions
                    mLRA.init();
                    mLRA.updateDeductions();
                    while( !mLRA.deductions().empty() )
                    {
                        #ifdef ICP_MODULE_DEBUG_1
                        cout << "Create deduction for: " << mLRA.deductions().back()->toString(false,0,"",true,true,true ) << endl;
                        #endif
                        const Formula* deduction = transformDeductions( mLRA.deductions().back() );
                        mCreatedDeductions.insert(deduction);
                        mLRA.rDeductions().pop_back();
                        addDeduction(deduction);
                        #ifdef ICP_MODULE_DEBUG_1
                        cout << "Passed deduction: " << deduction->toString(false,0,"",true,true,true ) << endl;
                        #endif
                    }
                    mIsIcpInitialized = true;
                }
                #ifdef ICP_MODULE_DEBUG_0
                cout << "[ICP] Assertion: " << constr << endl;
                #endif
                if( !(*_formula)->constraint().isBound() )
                {
                    addSubformulaToPassedFormula( *_formula, *_formula );
                    Module::assertSubformula( _formula );
                }

                // activate associated nonlinear contraction candidates
                if( !constr.lhs().isLinear() )
                {
                    activateNonlinearConstraint( *_formula );
                }
                // lookup corresponding linearization - in case the constraint is already linear, mReplacements holds the constraint as the linearized one
                auto replacementIt = mLinearizations.find( *_formula );
                assert( replacementIt != mLinearizations.end() );
                const Formula* replacementPtr = (*replacementIt).second;
                assert( replacementPtr->getType() == CONSTRAINT );
                if( replacementPtr->constraint().isBound() )
                {
                    // considered constraint is activated but has no slack variable -> it is a boundary constraint
                    mValidationFormula->push_back(replacementPtr);
                    #ifdef ICP_MODULE_DEBUG_0
                    cout << "[mLRA] Assert bound constraint: " << *replacementPtr << endl;
                    #endif
                    if( !mLRA.assertSubformula( --mValidationFormula->end() ) )
                    {
                        remapAndSetLraInfeasibleSubsets();
                        assert( !mInfeasibleSubsets.empty() );
                        return false;
                    }
                }
                else
                {
                    activateLinearConstraint( replacementPtr, *_formula );
                }
                return true;
            }
            default:
                return true;
        }
        return true;
    }

    void ICPModule::removeSubformula( ModuleInput::const_iterator _formula )
    {
        if( (*_formula)->getType() != CONSTRAINT )
        {
            Module::removeSubformula( _formula );
            return;
        }
        const Constraint* constr = (*_formula)->pConstraint();
        #ifdef ICP_MODULE_DEBUG_0
        cout << "[ICP] Remove Formula " << *constr << endl;
        #endif
        assert( constr->isConsistent() == 2 );
        auto rc = mReceivedConstraints.find( constr );
        if( rc != mReceivedConstraints.end() )
        {
            assert( rc->second > 0 );
            --(rc->second);
            if( rc->second > 0 )
            {
                Module::removeSubformula( _formula );
                return;
            }
            else
            {
                mReceivedConstraints.erase( rc );
            }
        }
        // is it nonlinear?
        auto iter = mNonlinearConstraints.find( constr );
        if( iter != mNonlinearConstraints.end() )
        {
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Nonlinear." << endl;
            #endif
            for( icp::ContractionCandidate* cc : iter->second )
            {
                // remove candidate if counter == 1, else decrement counter.
                assert( cc->isActive() );
                // remove origin, no matter if constraint is active or not
                cc->removeOrigin( *_formula );
                if( cc->activity() == 0 )
                {
                    // reset History to point before this candidate was used
                    icp::HistoryNode::set_HistoryNode nodes =  mHistoryRoot->findCandidates( cc );
                    // as the set is sorted ascending by id, we pick the node with the lowest id
                    if ( !nodes.empty() )
                    {
                        icp::HistoryNode* firstNode = (*nodes.begin())->parent();
                        if ( *firstNode == *mHistoryRoot )
                            firstNode = mHistoryRoot->addRight( new icp::HistoryNode( mHistoryRoot->intervals(), 2 ) );

                        setBox(firstNode);
                        mHistoryActual->reset();
                    }
                    // clean up icpRelevantCandidates
                    removeCandidateFromRelevant( cc );
                    mActiveNonlinearConstraints.erase( cc );
                    // find all linear replacements and deactivate them as well
                    for ( auto activeLinearIt = mActiveLinearConstraints.begin(); activeLinearIt != mActiveLinearConstraints.end(); )
                    {
                        if ( (*activeLinearIt)->hasOrigin(*_formula) )
                        {
                            assert( (*activeLinearIt)->activity() == 1 );
                            (*activeLinearIt)->removeOrigin(*_formula);
                            // clean up icpRelevantCandidates
                            removeCandidateFromRelevant( *activeLinearIt );
                            #ifdef ICP_MODULE_DEBUG_1                       
                            cout << "deactivate." << endl;
                            #endif
                            activeLinearIt = mActiveLinearConstraints.erase( activeLinearIt );
                        }
                        else
                            ++activeLinearIt;
                    }
                }
                else
                {
                    // directly decrement linear replacements
                    for ( auto activeLinearIt = mActiveLinearConstraints.begin(); activeLinearIt != mActiveLinearConstraints.end(); )
                    {
                        if ( (*activeLinearIt)->hasOrigin( *_formula ) )
                        {
                            #ifdef ICP_MODULE_DEBUG_1
                            cout << "Remove linear origin from candidate " << (*activeLinearIt)->id() << endl;
                            #endif
                            (*activeLinearIt)->removeOrigin(*_formula);
                            if( (*activeLinearIt)->activity() > 0 )
                            {
                                ++activeLinearIt;
                            }
                            else
                            {
                                // reset History to point before this candidate was used
                                icp::HistoryNode::set_HistoryNode nodes =  mHistoryRoot->findCandidates( *activeLinearIt );
                                // as the set is sorted ascending by id, we pick the node with the lowest id
                                if( !nodes.empty() )
                                {
                                    icp::HistoryNode* firstNode = (*nodes.begin())->parent();
                                    if ( *firstNode == *mHistoryRoot )
                                    {
                                        firstNode = mHistoryRoot->addRight( new icp::HistoryNode( mHistoryRoot->intervals(), 2 ) );
                                    }
                                    setBox(firstNode);
                                    mHistoryActual->reset();
                                }
                                #ifdef ICP_MODULE_DEBUG_1
                                cout << "Erase candidate from active." << endl;
                                #endif
                                // clean up icpRelevantCandidates
                                removeCandidateFromRelevant( *activeLinearIt );
                                activeLinearIt = mActiveLinearConstraints.erase( activeLinearIt );
                            }
                        }
                    }
                }
            }
        }

        // linear handling
        const vector<icp::ContractionCandidate*>& candidates = mCandidateManager->getInstance()->candidates();
        for( icp::ContractionCandidate* cc : candidates )
        {
            if( cc->isLinear() && cc->hasOrigin(*_formula) )
            {
                #ifdef ICP_MODULE_DEBUG_1
                cout << "Found linear candidate: ";
                candidateIt->print();
                cout << endl;
                #endif
                cc->removeOrigin( *_formula );
                assert( mActiveLinearConstraints.find( cc ) != mActiveLinearConstraints.end() );
                if( cc->activity() == 0  )
                {
                    // reset History to point before this candidate was used
                    icp::HistoryNode::set_HistoryNode nodes =  mHistoryRoot->findCandidates(cc);
                    // as the set is sorted ascending by id, we pick the node with the lowest id
                    if( !nodes.empty() )
                    {
                        icp::HistoryNode* firstNode = (*nodes.begin())->parent();
                        if ( *firstNode == *mHistoryRoot )
                            firstNode = mHistoryRoot->addRight(new icp::HistoryNode(mHistoryRoot->intervals(), 2));

                        setBox(firstNode);
                        mHistoryActual->reset();
                    }
                    // clean up icpRelevantCandidates
                    removeCandidateFromRelevant(cc);
                    mActiveLinearConstraints.erase( cc );
                }
            }
        }
        // remove constraint from mLRA module
        auto replacementIt = mLinearizations.find( *_formula );
        assert( replacementIt != mLinearizations.end() );
        auto validationFormulaIt = std::find( mValidationFormula->begin(), mValidationFormula->end(), (*replacementIt).first );
        if( validationFormulaIt != mValidationFormula->end() )
        {
            #ifdef ICP_MODULE_DEBUG_0
            cout << "[mLRA] remove " << *(*validationFormulaIt)->pConstraint() << endl;
            #endif
            mLRA.removeSubformula(validationFormulaIt);
            mValidationFormula->erase(validationFormulaIt);
        }
        Module::removeSubformula( _formula );
    }

    Answer ICPModule::isConsistent()
    {
        printIntervals(true);
        mInfeasibleSubsets.clear(); // Dirty! Normally this shouldn't be neccessary
        if( !mFoundSolution.empty() )
        {
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Found solution still feasible." << endl;
            #endif
            return foundAnswer( True );
        }
        mIsBackendCalled = false;

        // Debug Outputs of linear and nonlinear Tables
        #ifdef ICP_MODULE_DEBUG_0
        debugPrint();
        printAffectedCandidates();
        printIcpVariables();
        cout << "Id selected box: " << mHistoryRoot->id() << " Size subtree: " << mHistoryRoot->sizeSubtree() << endl;
        #endif
        Answer lraAnswer = Unknown;
        if( initialLinearCheck( lraAnswer ) )
        {
            return foundAnswer( lraAnswer );
        }
            
        #ifdef ICP_BOXLOG
        icpLog << "startTheoryCall";
        writeBox();
        #endif
        #ifdef ICP_MODULE_DEBUG_0
        printIntervals(true);
        cout << "---------------------------------------------" << endl;
        #endif
        for( ; ; )
        {
            bool splitOccurred = false;
            bool invalidBox = contractCurrentBox( splitOccurred );
            cout << endl << "contract to:" << endl;
            printIntervals(true);
            cout << endl;

            // when one interval is empty, we can skip validation and chose next box.
            if( !invalidBox )
            {
                #ifndef BOXMANAGEMENT
                if( splitOccurred )
                {
                    #ifdef ICP_MODULE_DEBUG_0
                    cout << "Return unknown, raise deductions for split." << endl;
                    #endif
                    return foundAnswer( Unknown );
                }
                #endif
                if( tryTestPoints() )
                {
                    return foundAnswer( True );
                }
                else
                {
                    // create Bounds and set them, add to passedFormula
                    pushBoundsToPassedFormula();
                    // call backends on found box
                    return foundAnswer( callBackends() );
                }
            }
            else // box contains no solution
            {
                #ifdef BOXMANAGEMENT
                // choose next box
                #ifdef ICP_MODULE_DEBUG_0
                cout << "Generated empty interval, Chose new box: " << endl;
                #endif
                if( mLastCandidate != NULL) // if there has been a candidate, the stateInfeasible set has to be created, otherwise it has been generated during checkBoxAgainstLinear...
                {
                    assert(mVariables.find(mLastCandidate->derivationVar()) != mVariables.end());
                    mHistoryActual->addInfeasibleVariable( mVariables.at(mLastCandidate->derivationVar()) );
                    if (mHistoryActual->rReasons().find(mLastCandidate->derivationVar()) != mHistoryActual->rReasons().end())
                    {
                        for( auto constraintIt = mHistoryActual->rReasons().at(mLastCandidate->derivationVar()).begin(); constraintIt != mHistoryActual->rReasons().at(mLastCandidate->derivationVar()).end(); ++constraintIt )
                            mHistoryActual->addInfeasibleConstraint(*constraintIt);
                    }
                }
                if( !chooseBox() )
                    return foundAnswer(False);
                #else
                #ifdef ICP_MODULE_DEBUG_0
                cout << "Whole box contains no solution! Return False." << endl;
                #endif
                // whole box forms infeasible subset
                mInfeasibleSubsets.push_back( createPremiseDeductions() );
                return foundAnswer( False );
                #endif
            }
        }
        assert( false ); // This should not happen!
        return foundAnswer( Unknown );
    }
    
    void ICPModule::addConstraint( const Formula* _formula )
    {
        assert( _formula->getType() == CONSTRAINT );
        assert( _formula->constraint().isConsistent() == 2 );
        const Constraint& constraint = _formula->constraint();
        auto linearization = mLinearizations.find( _formula );
        if( linearization == mLinearizations.end() )
        {
            const Polynomial constr = constraint.lhs();
            bool linear = false;
            // add original variables to substitution mapping
            for( auto var = constraint.variables().begin(); var != constraint.variables().end(); ++var )
            {
                if( mSubstitutions.find( *var ) == mSubstitutions.end() )
                {
                    assert( mVariables.find(*var) == mVariables.end() );
                    assert( mIntervals.find(*var) == mIntervals.end() );
                    mSubstitutions.insert( std::make_pair( *var, Polynomial(*var) ) );
                    mVariables.insert( std::make_pair( *var, getIcpVariable( *var, true, NULL ) ) ); // note that we have to set the lra variable later
                    mIntervals.insert( std::make_pair( *var, smtrat::DoubleInterval::unboundedInterval() ) );
                    mHistoryRoot->addInterval( *var, smtrat::DoubleInterval::unboundedInterval() );
                }
            }
            // actual preprocessing
            const Formula* linearFormula;
            if( constr.isLinear() )
            {
                linearFormula = _formula;
            }
            else
            {
                assert( mLinearizations.find( _formula ) == mLinearizations.end() );
                vector<Polynomial> temporaryMonomes;
                linear = icp::isLinear( _formula->pConstraint(), constr, temporaryMonomes );
                assert( !temporaryMonomes.empty() );
                Polynomial lhs = createNonlinearCCs( _formula->pConstraint(), temporaryMonomes );
                linearFormula = newFormula( newConstraint( lhs, constraint.relation() ) );
                #ifdef ICP_MODULE_DEBUG_0
                cout << "linearize constraint to   " << linearFormula->constraint() << endl;
                #endif
            }
            // store replacement for later comparison when asserting
            assert( mDeLinearizations.find( linearFormula ) == mDeLinearizations.end() );
            assert( mLinearizations.find( _formula ) == mLinearizations.end() );
            mDeLinearizations[linearFormula] = _formula;
            mLinearizations[_formula] = linearFormula;
            // inform internal LRAmodule of the linearized constraint
            mLRA.inform(linearFormula->pConstraint());
            const Constraint& linearizedConstraint = linearFormula->constraint();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "[mLRA] inform: " << linearizedConstraint << endl;
            #endif
            assert( linearizedConstraint.lhs().isLinear() );
            
            icp::IcpVariable* linearIcpVar = NULL;
            if( linearizedConstraint.isBound() )
            {
                // try to insert new icpVariable -> is original!
                const carl::Variable::Arg tmpVar = *constraint.variables().begin();
                const LRAVariable* slackvariable = mLRA.getSlackVariable(_formula->pConstraint());
                assert( slackvariable != NULL );
                assert( mSubstitutions.find( tmpVar ) != mSubstitutions.end() );
                linearIcpVar = getIcpVariable( tmpVar, mSubstitutions.find( tmpVar )->second.isLinear(), slackvariable );
            }
            else
            {
                createLinearCCs( linearFormula->pConstraint(), _formula );
            }
            
            // set the lra variables for the icp variables regarding variables (introduced and original ones)
            for( auto var = linearizedConstraint.variables().begin(); var != linearizedConstraint.variables().end(); ++var )
            {
                auto iter = mVariables.find( *var );
                assert( iter != mVariables.end() );
                if( iter->second->lraVar() == NULL )
                {
                    auto ovarIter = mLRA.originalVariables().find( *var );
                    if( ovarIter != mLRA.originalVariables().end() )
                    {
                        iter->second->setLraVar( ovarIter->second );
                    }
                }
            }
        }
    }
    
    icp::IcpVariable* ICPModule::getIcpVariable( carl::Variable::Arg _var, bool _original, const LRAVariable* _lraVar )
    {
        auto iter = mVariables.find( _var );
        if( iter != mVariables.end() )
        {
            return iter->second;
        }
        icp::IcpVariable* icpVar = new icp::IcpVariable( _var, _original, _lraVar );
        mVariables.insert( std::make_pair( _var, icpVar ) );
        return icpVar;
    }
    
    void ICPModule::activateNonlinearConstraint( const Formula* _formula )
    {
        assert( _formula->getType() == CONSTRAINT );
        auto iter = mNonlinearConstraints.find( _formula->pConstraint() );
        #ifdef ICP_MODULE_DEBUG_0
        cout << "[ICP] Assertion (nonlinear)" << _formula->constraint() <<  endl;
        cout << "mNonlinearConstraints.size: " << mNonlinearConstraints.size() << endl;
        cout << "Number Candidates: " << iter->second.size() << endl;
        #endif
        for( auto candidateIt = iter->second.begin(); candidateIt != iter->second.end(); ++candidateIt )
        {
            if( (*candidateIt)->activity() == 0 )
            {
                mActiveNonlinearConstraints.insert( *candidateIt );
                #ifdef ICP_MODULE_DEBUG_0
                cout << "[ICP] Activated candidate: ";
                (*candidateIt)->print();
                #endif
            }
            (*candidateIt)->addOrigin( _formula );
            #ifdef ICP_MODULE_DEBUG_0
            cout << "[ICP] Increased candidate count: ";
            (*candidateIt)->print();
            #endif
        }
    }
    
    void ICPModule::activateLinearConstraint( const Formula* _formula, const Formula* _origin )
    {
        assert( _formula->getType() == CONSTRAINT );
        const LRAVariable* slackvariable = mLRA.getSlackVariable( _formula->pConstraint() );
        assert( slackvariable != NULL );

        // lookup if contraction candidates already exist - if so, add origins
        auto iter = mLinearConstraints.find( slackvariable );
        assert( iter != mLinearConstraints.end() );
        for ( auto candidateIt = iter->second.begin(); candidateIt != iter->second.end(); ++candidateIt )
        {
            #ifdef ICP_MODULE_DEBUG_1
            cout << "[ICP] ContractionCandidates already exist: ";
            slackvariable->print();
            cout << ", Size Origins: " << (*candidateIt)->origin().size() << endl;
            cout << _formula << endl;
            (*candidateIt)->print();
            cout << "Adding origin." << endl;
            #endif
            // add origin
            (*candidateIt)->addOrigin( _origin );

            // set value in activeLinearConstraints
            if( (*candidateIt)->activity() == 0 )
            {
                mActiveLinearConstraints.insert( *candidateIt );
            }
        }

        // assert in mLRA
        mValidationFormula->push_back( _formula );

        if( !mLRA.assertSubformula(--mValidationFormula->end()) )
        {
            remapAndSetLraInfeasibleSubsets();
        }
        #ifdef ICP_MODULE_DEBUG_0
        cout << "[mLRA] Assert " << *_formula << endl;
        #endif
    }
    
    bool ICPModule::initialLinearCheck( Answer& _answer )
    {
        #ifdef ICP_MODULE_DEBUG_0
        cout << "Initial linear check:" << endl;
        #endif
        // call mLRA to check linear feasibility
        mLRA.clearDeductions();
        mLRA.rReceivedFormula().updateProperties();
        _answer = mLRA.isConsistent();
        
        // catch deductions
        mLRA.updateDeductions();
        while( !mLRA.deductions().empty() )
        {
            #ifdef ICP_MODULE_DEBUG_1
            cout << "Create deduction for: " << *mLRA.deductions().back() << endl;
            #endif
            const Formula* deduction = transformDeductions(mLRA.deductions().back());
            mLRA.rDeductions().pop_back();
            addDeduction(deduction);
            #ifdef ICP_MODULE_DEBUG_1   
            cout << "Passed deduction: " << *deduction << endl;
            #endif
        }
        mLRA.clearDeductions();
        if( _answer == False )
        {
            // remap infeasible subsets to original constraints
            remapAndSetLraInfeasibleSubsets();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "LRA: " << _answer << endl;
            #endif
            return true;
        }
        else if( _answer == Unknown )
        {
            #ifdef ICP_MODULE_DEBUG_0
            mLRA.printReceivedFormula();
            cout << "LRA: " << _answer << endl;
            #endif
            return true;
        }
        else if( mActiveNonlinearConstraints.empty() ) // _answer == True, but no nonlinear constraints -> linear solution is a solution
        {
            #ifdef ICP_MODULE_DEBUG_0
            cout << "LRA: " << _answer << endl;
            #endif
            mFoundSolution = mLRA.getRationalModel();
            return true;
        }
        else // _answer == True
        {
            // get intervals for initial variables
            EvalIntervalMap tmp = mLRA.getVariableBounds();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Newly obtained Intervals: " << endl;
            #endif
            for ( auto constraintIt = tmp.begin(); constraintIt != tmp.end(); ++constraintIt )
            {
                #ifdef ICP_MODULE_DEBUG_0
                cout << (*constraintIt).first << ": " << (*constraintIt).second << endl;
                #endif
                if (mVariables.find((*constraintIt).first) != mVariables.end())
                {
                    Interval tmp = (*constraintIt).second;
                    mHistoryRoot->addInterval((*constraintIt).first, smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType()) );
                    mIntervals[(*constraintIt).first] = smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType() );
                    mVariables.at((*constraintIt).first)->setUpdated();
                }
            }
            
            // get intervals for slackvariables
            const LRAModule::ExVariableMap slackVariables = mLRA.slackVariables();
            for( auto slackIt = slackVariables.begin(); slackIt != slackVariables.end(); ++slackIt )
            {
                std::map<const LRAVariable*, ContractionCandidates>::iterator linIt = mLinearConstraints.find((*slackIt).second);
                if ( linIt != mLinearConstraints.end() )
                {
                    // dirty hack: expect lhs to be set and take first item of set of CCs --> Todo: Check if it is really set in the constructors of the CCs during inform and assert
                    Interval tmp = (*slackIt).second->getVariableBounds();
                    // keep root updated about the initial box.
                    mHistoryRoot->rIntervals()[(*(*linIt).second.begin())->lhs()] = smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType());
                    mIntervals[(*(*linIt).second.begin())->lhs()] = smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType());
                    #ifdef ICP_MODULE_DEBUG_1
                    cout << "Added interval (slackvariables): " << (*(*linIt).second.begin())->lhs() << " " << tmp << endl;
                    #endif
                }
            }
            // temporary solution - an added linear constraint might have changed the box.
            setBox(mHistoryRoot);
            mHistoryRoot->rReasons().clear();
            mHistoryRoot->rStateInfeasibleConstraints().clear();
            mHistoryRoot->rStateInfeasibleVariables().clear();
            mHistoryActual = mHistoryActual->addRight( new icp::HistoryNode( mIntervals, 2 ) );
            mCurrentId = mHistoryActual->id();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Id actual box: " << mHistoryActual->id() << " Size subtree: " << mHistoryActual->sizeSubtree() << endl;
            #endif
            return false;
        }
    }
    
    bool ICPModule::contractCurrentBox( bool& _splitOccurred )
    {
        bool invalidBox = false;
        mLastCandidate = NULL;
        double relativeContraction = 1;
        double absoluteContraction = 0;
        std::pair<bool,carl::Variable> didSplit = std::make_pair(false, carl::Variable::NO_VARIABLE);

        for( ; ; )
        {
            #ifndef BOXMANAGEMENT
            while(!mBoxStorage.empty())
                mBoxStorage.pop();

            icp::set_icpVariable icpVariables;
            Variables originalRealVariables;
            mpReceivedFormula->realValuedVars(originalRealVariables);
            for( auto variablesIt = originalRealVariables.begin(); variablesIt != originalRealVariables.end(); ++variablesIt )
            {
                assert(mVariables.count(*variablesIt) > 0);
                icpVariables.insert( (*(mVariables.find(*variablesIt))).second );
            }
            PointerSet<Formula> box = variableReasonHull(icpVariables);
            mBoxStorage.push(box);
//            cout << "ADD TO BOX!" << endl;
            #endif
            #ifdef ICP_MODULE_DEBUG_0
            cout << "********************** [ICP] Contraction **********************" << endl;
            cout << "Subtree size: " << mHistoryRoot->sizeSubtree() << endl;
            mHistoryActual->print();
            #endif
            #ifdef ICP_BOXLOG
            icpLog << "startContraction";
            writeBox();
            #endif
            #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
            Formula* negatedContraction = new Formula(*mpReceivedFormula);
            GiNaCRA::evaldoubleintervalmap tmp = GiNaCRA::evaldoubleintervalmap();
            for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
                tmp.insert((*constraintIt));

            PointerSet<Formula> boundaryConstraints = createConstraintsFromBounds(tmp);
            for ( auto boundaryConstraint = boundaryConstraints.begin(); boundaryConstraint != boundaryConstraints.end(); ++boundaryConstraint )
                negatedContraction->addSubformula(*boundaryConstraint);
            #endif
            // prepare IcpRelevantCandidates
//            activateLinearEquations(); // TODO (Florian): do something alike again
            fillCandidates();
            _splitOccurred = false;

            while ( !mIcpRelevantCandidates.empty() && !_splitOccurred )
            {
                #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
                mCheckContraction = new Formula(*mpReceivedFormula);

                GiNaCRA::evaldoubleintervalmap tmp = GiNaCRA::evaldoubleintervalmap();
                for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
                    tmp.insert((*constraintIt));

                PointerSet<Formula> boundaryConstraints = createConstraintsFromBounds(tmp);
                for ( auto boundaryConstraint = boundaryConstraints.begin(); boundaryConstraint != boundaryConstraints.end(); ++boundaryConstraint )
                    mCheckContraction->addSubformula(*boundaryConstraint);
                #endif

                icp::ContractionCandidate* candidate = chooseContractionCandidate();
                assert(candidate != NULL);
                candidate->calcDerivative();
                relativeContraction = -1;
                absoluteContraction = 0;
                _splitOccurred = contraction( candidate, relativeContraction, absoluteContraction );
                #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
                if ( !_splitOccurred && relativeContraction != 0 )
                {
                    GiNaCRA::evaldoubleintervalmap tmp = GiNaCRA::evaldoubleintervalmap();
                    for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
                        tmp.insert((*constraintIt));

                    PointerSet<Formula> contractedBox = createConstraintsFromBounds(tmp);
                    Formula* negBox = new Formula(NOT);
                    Formula* boxConjunction = new Formula(AND);
                    for ( auto formulaIt = contractedBox.begin(); formulaIt != contractedBox.end(); ++formulaIt )
                        boxConjunction->addSubformula(*formulaIt);

                    negBox->addSubformula(boxConjunction);
                    mCheckContraction->addSubformula(negBox);
                    addAssumptionToCheck(*mCheckContraction,false,"SingleContractionCheck");
                }
                mCheckContraction->clear();
                delete mCheckContraction;
                #endif

                // catch if new interval is empty -> we can drop box and chose next box
                if ( mIntervals.at(candidate->derivationVar()).isEmpty() )
                {
                    #ifdef ICP_MODULE_DEBUG_0
                    cout << "GENERATED EMPTY INTERVAL, Drop Box: " << endl;
                    #endif
                    mLastCandidate = candidate;
                    invalidBox = true;
                    break;
                }

                if ( relativeContraction > 0 )
                {
                    std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.find(candidate->derivationVar());
                    assert(icpVar != mVariables.end());
                    (*icpVar).second->setUpdated();
                    mLastCandidate = candidate;
                }

                // update weight of the candidate
                removeCandidateFromRelevant(candidate);
                candidate->setPayoff(relativeContraction);
                candidate->calcRWA();

                // only add nonlinear CCs as linear CCs should only be used once
                if ( !candidate->isLinear() )
                {
                    addCandidateToRelevant(candidate);
                }

                assert(mIntervals.find(candidate->derivationVar()) != mIntervals.end() );
                #ifdef ICP_CONSIDER_WIDTH
                if ( (relativeContraction < mContractionThreshold && !_splitOccurred) || mIntervals.at(candidate->derivationVar()).diameter() <= mTargetDiameter )
                #else
                if ( (absoluteContraction < mContractionThreshold && !_splitOccurred) )
                #endif
                {
                    removeCandidateFromRelevant(candidate);
                }
                #ifdef ICP_CONSIDER_WIDTH
                else if ( relativeContraction >= mContractionThreshold )
                #else
                else if ( absoluteContraction >= mContractionThreshold )
                #endif
                {
                    /**
                     * make sure all candidates which contain the variable
                     * of which the interval has significantly changed are
                     * contained in mIcpRelevantCandidates.
                     */
                    std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.find(candidate->derivationVar());
                    assert(icpVar != mVariables.end());
                    for ( auto candidateIt = (*icpVar).second->candidates().begin(); candidateIt != (*icpVar).second->candidates().end(); ++candidateIt )
                    {
                        bool toAdd = true;
                        for ( auto relevantCandidateIt = mIcpRelevantCandidates.begin(); relevantCandidateIt != mIcpRelevantCandidates.end(); ++relevantCandidateIt )
                        {
                            if ( (*relevantCandidateIt).second == (*candidateIt)->id() )
                                toAdd = false;
                        }
                        #ifdef ICP_CONSIDER_WIDTH
                        if ( toAdd && (*candidateIt)->isActive() && mIntervals.at((*candidateIt)->derivationVar()).diameter() > mTargetDiameter )
                        #else
                        if( toAdd && (*candidateIt)->isActive() )
                        #endif
                        {
                            addCandidateToRelevant(*candidateIt);
                        }
                    }
                    #ifdef ICP_BOXLOG
                    icpLog << "contraction; \n";
                    #endif
                }

                #ifdef ICP_CONSIDER_WIDTH
                bool originalAllFinished = true;
                Variables originalRealVariables;
                mpReceivedFormula->realValuedVars(originalRealVariables);
                for( auto varIt = originalRealVariables.begin(); varIt != originalRealVariables.end(); ++varIt )
                {
                    if( mIntervals.find(*varIt) != mIntervals.end() )
                    {
                        if( mIntervals.at(*varIt).diameter() > mTargetDiameter )
                        {
                            originalAllFinished = false;
                            break;
                        }
                    }
                }
                if( originalAllFinished )
                {
                    mIcpRelevantCandidates.clear();
                    break;
                }
                #endif
            } //while ( !mIcpRelevantCandidates.empty() && !_splitOccurred)
            // verify if the box is already invalid
            if (!invalidBox && !_splitOccurred)
            {
                invalidBox = !checkBoxAgainstLinearFeasibleRegion();
                #ifdef ICP_MODULE_DEBUG_0
                cout << "Invalid against linear region: " << (invalidBox ? "yes!" : "no!") << endl;
                #endif
                #ifdef ICP_BOXLOG
                if ( invalidBox )
                {
                    icpLog << "invalid Post Contraction; \n";
                }
                #endif
                // do a quick test with one point.
//                if( !invalidBox )
//                {
//                    EvalRationalMap rationals;
//                    std::map<carl::Variable, double> values = createModel();
//                    for(auto value : values)
//                    {
//                        rationals.insert(std::make_pair(value.first, carl::rationalize<Rational>(value.second)));
//                    }
//                    unsigned result = mpReceivedFormula->satisfiedBy(rationals);
//                    if ( result == 1 )
//                    {
//                        return foundAnswer(True);
//                    }
//                }
            }
            #ifdef ICP_BOXLOG
            else
            {
                icpLog << "contract to emp; \n";
            }
            #endif
            #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
            if ( !_splitOccurred && !invalidBox )
            {
                GiNaCRA::evaldoubleintervalmap tmp = GiNaCRA::evaldoubleintervalmap();
                for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
                    tmp.insert((*constraintIt));

                PointerSet<Formula> contractedBox = createConstraintsFromBounds(tmp);
                Formula* negConstraint = new Formula(NOT);
                Formula* conjunction = new Formula(AND);
                for ( auto formulaIt = contractedBox.begin(); formulaIt != contractedBox.end(); ++formulaIt )
                    conjunction->addSubformula(*formulaIt);

                negConstraint->addSubformula(conjunction);
                negatedContraction->addSubformula(negConstraint);
                addAssumptionToCheck(*negatedContraction,false,"ICPContractionCheck");
            }
            negatedContraction->clear();
            delete negatedContraction;
            #endif
            didSplit.first = false;
            if( invalidBox || _splitOccurred || mIcpRelevantCandidates.empty() ) // relevantCandidates is not empty, if we got new bounds from LRA during boxCheck
            {
                // perform splitting if possible
                if( !invalidBox && !_splitOccurred )
                    didSplit = checkAndPerformSplit();
                if( didSplit.first || (_splitOccurred && !invalidBox) )
                {
                    #ifdef ICP_BOXLOG
                    icpLog << "split size subtree; " << mHistoryRoot->sizeSubtree() << "\n";
                    #endif
                    #ifdef ICP_MODULE_DEBUG_1
                    cout << "Size subtree: " << mHistoryActual->sizeSubtree() << " \t Size total: " << mHistoryRoot->sizeSubtree() << endl;
                    #endif
                    #ifdef BOXMANAGEMENT
                    invalidBox = false;
                    #else
                    _splitOccurred = true;
                    return invalidBox;
                    #endif
                }
                else
                    return invalidBox;

                #ifdef ICP_MODULE_DEBUG_0
                cout << "empty: " << invalidBox << "  didSplit: " << didSplit.first << endl;
                #endif
            }
        }
        assert( false ); // should not happen
        return invalidBox;
    }
    
    Answer ICPModule::callBackends()
    {
        #ifdef ICP_MODULE_DEBUG_0
        cout << "[ICP] created passed formula." << endl;
        printPassedFormula();
        #endif
        #ifdef ICP_BOXLOG
        icpLog << "backend";
        writeBox();
        #endif
        ++mCountBackendCalls;
        Answer a = runBackends();
        mIsBackendCalled = true;
        #ifdef ICP_MODULE_DEBUG_0
        cout << "[ICP] Done running backends:" << a << endl;
        #endif
        if( a == False )
        {
            assert(infeasibleSubsets().empty());
            bool isBoundInfeasible = false;
            bool isBound = false;

            vector<Module*>::const_iterator backend = usedBackends().begin();
            while( backend != usedBackends().end() )
            {
                assert( !(*backend)->infeasibleSubsets().empty() );
                for( vec_set_const_pFormula::const_iterator infsubset = (*backend)->infeasibleSubsets().begin();
                        infsubset != (*backend)->infeasibleSubsets().end(); ++infsubset )
                {
                    for( auto subformula = infsubset->begin(); subformula != infsubset->end(); ++subformula )
                    {
                        isBound = false;
                        std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.begin();
                        for ( ; icpVar != mVariables.end(); ++icpVar )
                        {
                            if( (*icpVar).second->isOriginal() && (*icpVar).second->isExternalBoundsSet() != icp::Updated::NONE )
                            {
                                assert( (*icpVar).second->isExternalUpdated() != icp::Updated::NONE );
                                if ( (*subformula) == (*(*icpVar).second->externalLeftBound()) || (*subformula) == (*(*icpVar).second->externalRightBound()) )
                                {
                                    isBound = true;
                                    isBoundInfeasible = true;
                                    assert(mVariables.find( *(*subformula)->constraint().variables().begin() ) != mVariables.end() );
                                    mHistoryActual->addInfeasibleVariable( mVariables.at( *(*subformula)->constraint().variables().begin() ) );
                                    break;
                                }
                            }
                        }
                        if(!isBound)
                        {
                            if (mInfeasibleSubsets.empty())
                            {
                                PointerSet<Formula> infeasibleSubset;
                                infeasibleSubset.insert(*subformula);
                                mInfeasibleSubsets.insert(mInfeasibleSubsets.begin(), infeasibleSubset);
                            }
                            else
                                (*mInfeasibleSubsets.begin()).insert(*subformula);
                        }
                    }
                }
                break;
            }
            if ( isBoundInfeasible )
            {
                // set stateInfeasibleSubset
                assert(!mInfeasibleSubsets.empty());
                for (auto infSetIt = (*mInfeasibleSubsets.begin()).begin(); infSetIt != (*mInfeasibleSubsets.begin()).end(); ++infSetIt )
                {
                    if( (*infSetIt)->pConstraint()->isBound() )
                    {
                        assert( mVariables.find( *(*infSetIt)->constraint().variables().begin() ) != mVariables.end() );
//                                        mHistoryActual->addInfeasibleVariable( mVariables.at((*(*infSetIt)->constraint().variables().begin()).first) );
//                                        cout << "Added infeasible Variable." << endl;
                    }
                    else
                    {
                        mHistoryActual->addInfeasibleConstraint((*infSetIt)->pConstraint());
//                                        cout << "Added infeasible Constraint." << endl;
                    }

                }
                // clear infeasible subsets
                mInfeasibleSubsets.clear();
                #ifdef BOXMANAGEMENT
                #ifdef ICP_MODULE_DEBUG_0
                cout << "InfSet of Backend contained bound, Chose new box: " << endl;
                #endif
                if( !chooseBox() )
                    return foundAnswer(False);
                #else
                mInfeasibleSubsets.push_back(createPremiseDeductions());
                return Unknown;
                #endif
            }
            else
            {
                mHistoryActual->propagateStateInfeasibleConstraints();
                mHistoryActual->propagateStateInfeasibleVariables();
                mInfeasibleSubsets.clear();
                mInfeasibleSubsets.push_back(collectReasons(mHistoryRoot));
                // printInfeasibleSubsets();
                return False;
            }
        }
        else // if answer == true or answer == unknown
        {
            mHistoryActual->propagateStateInfeasibleConstraints();
            mHistoryActual->propagateStateInfeasibleVariables();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Backend: " << ANSWER_TO_STRING( a ) << endl;
            #endif
            return a;
        }
    }
        
    Polynomial ICPModule::createNonlinearCCs( const Constraint* _constraint, const vector<Polynomial>& _tempMonomes )
    {
        Polynomial linearizedConstraint = smtrat::ZERO_POLYNOMIAL;
        ContractionCandidates ccs;
        // Create contraction candidate object for every possible derivation variable
        for( auto& monom : _tempMonomes )
        {
            auto iter = mVariableLinearizations.find( monom );
            if( iter == mVariableLinearizations.end() )
            {
                // create mLinearzations entry
                Variables variables;
                monom.gatherVariables( variables );
                bool hasRealVar = false;
                for( auto var : variables )
                {
                    if( var.getType() == carl::VariableType::VT_REAL )
                    {
                        hasRealVar = true;
                        break;
                    }
                }
                carl::Variable newVar = hasRealVar ? newAuxiliaryRealVariable() : newAuxiliaryIntVariable();
                mVariableLinearizations.insert( std::make_pair( monom, newVar ) );
                mSubstitutions.insert( std::make_pair( newVar, monom ) );
                assert( mVariables.find( newVar ) == mVariables.end() );
                icp::IcpVariable* icpVar = getIcpVariable( newVar, false, NULL );
                mVariables.insert( std::make_pair( newVar, icpVar ) );
                assert( mIntervals.find( newVar ) == mIntervals.end() );
                mIntervals.insert( std::make_pair( newVar, smtrat::DoubleInterval::unboundedInterval() ) );
                mHistoryRoot->addInterval( newVar, smtrat::DoubleInterval::unboundedInterval() );
                #ifdef ICP_MODULE_DEBUG_0
                cout << "New replacement: " << monom << " -> " << mVariableLinearizations.at(monom) << endl;
                #endif

                const Polynomial rhs = monom - newVar;
                for( auto varIndex = variables.begin(); varIndex != variables.end(); ++varIndex )
                {
                    if( mContractors.find(rhs) == mContractors.end() )
                    {
                        mContractors.insert(std::make_pair(rhs, Contractor<carl::SimpleNewton>(rhs)));
                    }
                    const Constraint* tmp = newConstraint( rhs, Relation::EQ );
                    icp::ContractionCandidate* tmpCandidate = mCandidateManager->getInstance()->createCandidate( newVar, rhs, tmp, *varIndex, mContractors.at( rhs ) );
                    ccs.insert( ccs.end(), tmpCandidate );
                    tmpCandidate->setNonlinear();
                    auto tmpIcpVar = mVariables.find( newVar );
                    assert( tmpIcpVar != mVariables.end() );
                    tmpIcpVar->second->addCandidate( tmpCandidate );
                }
                // add one candidate for the replacement variable
                const Constraint* tmp = newConstraint( rhs, Relation::EQ );
                icp::ContractionCandidate* tmpCandidate = mCandidateManager->getInstance()->createCandidate( newVar, rhs, tmp, newVar, mContractors.at( rhs ) );
                tmpCandidate->setNonlinear();
                icpVar->addCandidate( tmpCandidate );
                ccs.insert( ccs.end(), tmpCandidate );
            }
            else // already existing replacement/substitution/linearization
            {
                #ifdef ICP_MODULE_DEBUG_1
                cout << "Existing replacement: " << monom << " -> " << mVariableLinearizations.at(monom) << endl;
                #endif
                auto iterB = mVariables.find( iter->second );
                assert( iterB != mVariables.end() );
                ccs.insert( iterB->second->candidates().begin(), iterB->second->candidates().end() );
            }
        }
        for( auto monomialIt = _constraint->lhs().begin(); monomialIt != _constraint->lhs().end(); ++monomialIt )
        {
            if( (*monomialIt)->monomial() == NULL || (*monomialIt)->monomial()->isAtMostLinear() )
            {
                linearizedConstraint += **monomialIt;
            }
            else
            {
                assert( mVariableLinearizations.find(Polynomial(*(*monomialIt)->monomial())) != mVariableLinearizations.end() );
                linearizedConstraint += (*monomialIt)->coeff() * (*mVariableLinearizations.find( Polynomial(*(*monomialIt)->monomial() ))).second;
            }
        }
        mNonlinearConstraints.insert( pair<const Constraint*, ContractionCandidates>( _constraint, ccs ) );
        return linearizedConstraint;
    }
    
    void ICPModule::createLinearCCs( const Constraint* _constraint, const Formula* _origin )
    {
        assert( _constraint->lhs().isLinear() );
        const LRAVariable* slackvariable = mLRA.getSlackVariable( _constraint );
        assert( slackvariable != NULL );
        if( mLinearConstraints.find( slackvariable ) == mLinearConstraints.end() )
        {
            Variables variables = _constraint->variables();
            bool hasRealVar = false;
            for( carl::Variable::Arg var : variables )
            {
                if( var.getType() == carl::VariableType::VT_REAL )
                {
                    hasRealVar = true;
                    break;
                }
            }
            carl::Variable newVar = hasRealVar ? newAuxiliaryRealVariable() : newAuxiliaryIntVariable();
            variables.insert( newVar );
            mSubstitutions.insert( std::make_pair( newVar, Polynomial( newVar ) ) );
            assert( mVariables.find( newVar ) == mVariables.end() );
            icp::IcpVariable* icpVar = getIcpVariable( newVar, false, slackvariable );
            mVariables.insert( std::make_pair( newVar, icpVar ) );
            assert( mIntervals.find( newVar ) == mIntervals.end() );
            mIntervals.insert( std::make_pair( newVar, smtrat::DoubleInterval::unboundedInterval() ) );
            mHistoryRoot->addInterval( newVar, smtrat::DoubleInterval::unboundedInterval() );

            const Polynomial rhs = slackvariable->expression() - newVar;
            const Constraint* tmpConstr = newConstraint( rhs, Relation::EQ );
            auto iter = mContractors.find( rhs );
            if( iter == mContractors.end() )
            {
                iter = mContractors.insert( std::make_pair( rhs, Contractor<carl::SimpleNewton>(rhs) ) ).first;
            }

            // Create candidates for every possible variable:
            for( auto var = variables.begin(); var != variables.end(); ++var )
            {   
                icp::ContractionCandidate* newCandidate = mCandidateManager->getInstance()->createCandidate( newVar, rhs, tmpConstr, *var, iter->second, _origin );

                // ensure that the created candidate is set as linear
                newCandidate->setLinear();
                #ifdef ICP_MODULE_DEBUG_1
                cout << "[ICP] Create & activate candidate: ";
                newCandidate->print();
                slackvariable->print();
                #endif
                icpVar->addCandidate( newCandidate );
            }
            mLinearConstraints.insert( pair<const LRAVariable*, ContractionCandidates>( slackvariable, icpVar->candidates() ) );
        }
    }
    
    void ICPModule::initiateWeights()
    {
//        std::map<const Constraint*, ContractionCandidates>::iterator constrIt;
//        ContractionCandidates::iterator   varIt;
//        double                   minDiameter = 0;
//        double maxDiameter = 0;
//        bool                     minSet = false;
//        bool                     maxSet = false;
//        vector<carl::Variable>           variables = vector<carl::Variable>();
//
//        // calculate Jacobian for initial box
//        for( constrIt = mNonlinearConstraints.begin(); constrIt != mNonlinearConstraints.end(); constrIt++ )
//        {
//            std::set<icp::ContractionCandidate*> tmp = constrIt->second;
//
//            minSet = false;
//            maxSet = false;
//
//            for( varIt = tmp.begin(); varIt != tmp.end(); varIt++ )
//            {
//                (*varIt)->calcDerivative();
//
//                variables.clear();
//                const Polynomial term = (*varIt)->derivative();
//                mIcp.searchVariables( term, &variables );
//
//                if( !minSet )
//                {
//                    minDiameter = mIntervals[(*varIt)->derivationVar()].upper() - mIntervals[(*varIt)->derivationVar()].upper();
//                }
//                else
//                {
//                    minDiameter = mIntervals[(*varIt)->derivationVar()].upper() - mIntervals[(*varIt)->derivationVar()].upper() < minDiameter
//                                  ? mIntervals[(*varIt)->derivationVar()].upper() - mIntervals[(*varIt)->derivationVar()].upper() : minDiameter;
//                }
//
//                if( !maxSet )
//                {
//                    maxDiameter = mIntervals[(*varIt)->derivationVar()].upper() - mIntervals[(*varIt)->derivationVar()].upper();
//                }
//                else
//                {
//                    maxDiameter = mIntervals[(*varIt)->derivationVar()].upper() - mIntervals[(*varIt)->derivationVar()].upper() > maxDiameter
//                                  ? mIntervals[(*varIt)->derivationVar()].upper() - mIntervals[(*varIt)->derivationVar()].upper() : maxDiameter;
//                }
//            }
//        }
    }
    
//    void ICPModule::activateLinearEquations()
//    {
//        for( auto candidatesIt = mLinearConstraints.begin(); candidatesIt != mLinearConstraints.end(); ++candidatesIt )
//        {
//            ContractionCandidates candidates = (*candidatesIt).second;
//            for( auto ccIt = candidates.begin(); ccIt != candidates.end(); ++ccIt )
//            {
//                if( (*ccIt)->constraint()->relation() == Relation::EQ )
//                {
//                    (*ccIt)->activate();
//                }
//            }
//        }
//    }
    
    void ICPModule::fillCandidates()
    {
        // fill mIcpRelevantCandidates with the nonlinear contractionCandidates
        for ( icp::ContractionCandidate* nonlinearIt : mActiveNonlinearConstraints )
        {
            // check that assertions have been processed properly
            assert( (*nonlinearIt).activity() == (*nonlinearIt).origin().size() );
            assert( mIntervals.find((*nonlinearIt).derivationVar()) != mIntervals.end() );
#ifdef ICP_CONSIDER_WIDTH
            if ( mIntervals.at((*nonlinearIt).derivationVar()).diameter() > mTargetDiameter || mIntervals.at((*nonlinearIt).derivationVar()).diameter() == -1 )
#else
            if ( mIntervals.at((*nonlinearIt).derivationVar()).diameter() > 0 || mIntervals.at((*nonlinearIt).derivationVar()).diameter() == -1 )
#endif
            {
                // only add if not already existing
                addCandidateToRelevant( nonlinearIt );
            }
            else // the candidate is not relevant -> delete from icpRelevantCandidates
            {
                removeCandidateFromRelevant(nonlinearIt);
            }
        }
        // fill mIcpRelevantCandidates with the active linear contractionCandidates
        for ( icp::ContractionCandidate* linearIt : mActiveLinearConstraints )
        {
            // check that assertions have been processed properly
            assert( (*linearIt).activity() == (*linearIt).origin().size() );
            assert( mIntervals.find((*linearIt).derivationVar()) != mIntervals.end() );
#ifdef ICP_CONSIDER_WIDTH
            if ( (*linearIt).isActive() && ( mIntervals.at((*linearIt).derivationVar()).diameter() > mTargetDiameter || mIntervals.at((*linearIt).derivationVar()).diameter() == -1 ) )
#else
            if ( (*linearIt).isActive() && ( mIntervals.at((*linearIt).derivationVar()).diameter() > 0 || mIntervals.at((*linearIt).derivationVar()).diameter() == -1 ) )
#endif
            {
                addCandidateToRelevant( linearIt );
            }
            else // the candidate is not relevant -> delete from icpRelevantCandidates
            {
                removeCandidateFromRelevant( linearIt );
            }
        }
    }
    
    bool ICPModule::addCandidateToRelevant(icp::ContractionCandidate* _candidate)
    {
        if ( _candidate->isActive() )
        {
            assert( mIcpRelevantCandidates.find( std::pair<double, unsigned>( _candidate->lastRWA(), _candidate->id() ) ) == mIcpRelevantCandidates.end() );
            std::pair<double, unsigned> target(_candidate->RWA(), _candidate->id());
            if ( mIcpRelevantCandidates.find(target) == mIcpRelevantCandidates.end() )
            {
                #ifdef ICP_MODULE_DEBUG_0
                cout << "add to relevant candidates: " << (*_candidate).rhs() << endl;
                cout << "   id: " << (*_candidate).id() << endl;
                #endif
                mIcpRelevantCandidates.insert(target);
                _candidate->updateLastRWA();
                return true;
            }
        }
        return false;
    }
    
    bool ICPModule::removeCandidateFromRelevant(icp::ContractionCandidate* _candidate)
    {
        std::pair<double, unsigned> target(_candidate->lastRWA(), _candidate->id());
        auto iter = mIcpRelevantCandidates.find( target );
        if( iter != mIcpRelevantCandidates.end() )
        {
            #ifdef ICP_MODULE_DEBUG_0
            cout << "remove from relevant candidates due to diameter: " << (*_candidate).rhs() << endl;
            cout << "   id: " << (*_candidate).id() << " , Diameter: " << mIntervals[(*_candidate).derivationVar()].diameter() << endl;
            #endif
            mIcpRelevantCandidates.erase(iter);
            return true;
        }
        return false;
    }
    				
    void ICPModule::updateRelevantCandidates(carl::Variable _var, double _relativeContraction)
    {
        // update all candidates which contract in the dimension in which the split has happened
        std::set<icp::ContractionCandidate*> updatedCandidates;
        // iterate over all affected constraints
        std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.find(_var);
        assert(icpVar != mVariables.end());
        for ( auto candidatesIt = (*icpVar).second->candidates().begin(); candidatesIt != (*icpVar).second->candidates().end(); ++candidatesIt)
        {
            if ( (*candidatesIt)->isActive() )
            {
                unsigned id = (*candidatesIt)->id();
                // search if candidate is already contained - erase if, else do nothing
                removeCandidateFromRelevant(*candidatesIt);

                // create new tuple for mIcpRelevantCandidates
                mCandidateManager->getInstance()->getCandidate(id)->setPayoff(_relativeContraction );
                mCandidateManager->getInstance()->getCandidate(id)->calcRWA();
                updatedCandidates.insert(*candidatesIt);
            }
        }
        // re-insert tuples into icpRelevantCandidates
        for ( auto candidatesIt = updatedCandidates.begin(); candidatesIt != updatedCandidates.end(); ++candidatesIt )
        {
            #ifdef ICP_CONSIDER_WIDTH
            if ( mIntervals.at(_var).diameter() > mTargetDiameter )
            #endif
            {
                addCandidateToRelevant(*candidatesIt);
            }
        }
    }
    
    icp::ContractionCandidate* ICPModule::chooseContractionCandidate()
    {
        assert(!mIcpRelevantCandidates.empty());
        // as the map is sorted ascending, we can simply pick the last value
        for( auto candidateIt = mIcpRelevantCandidates.rbegin(); candidateIt != mIcpRelevantCandidates.rend(); ++candidateIt )
        {
            if( mCandidateManager->getInstance()->getCandidate((*candidateIt).second)->isActive() )//&& mIntervals[mCandidateManager->getInstance()->getCandidate((*candidateIt).second)->derivationVar()].diameter() != 0 )
            {
                #ifdef ICP_MODULE_DEBUG_0
                cout << "Chose Candidate: ";
                mCandidateManager->getInstance()->getCandidate((*candidateIt).second)->print();
                cout << endl;
                #endif
                return mCandidateManager->getInstance()->getCandidate((*candidateIt).second);
            }
        }
        return NULL;
    }
    
    bool ICPModule::contraction( icp::ContractionCandidate* _selection, double& _relativeContraction, double& _absoluteContraction )
    {
        smtrat::DoubleInterval resultA = smtrat::DoubleInterval();
        smtrat::DoubleInterval resultB = smtrat::DoubleInterval();
        bool                   splitOccurred = false;

        // check if derivative is already calculated
        if(_selection->derivative() == 0)
            _selection->calcDerivative();

        const Polynomial               constr     = _selection->rhs();
        const Polynomial               derivative = _selection->derivative();
        const carl::Variable           variable   = _selection->derivationVar();
        assert(mIntervals.find(variable) != mIntervals.end());
        double                 originalDiameter = mIntervals.at(variable).diameter();
        bool originalUnbounded = ( mIntervals.at(variable).lowerBoundType() == carl::BoundType::INFTY || mIntervals.at(variable).upperBoundType() == carl::BoundType::INFTY );
        smtrat::DoubleInterval originalInterval = mIntervals.at(variable);
        
        splitOccurred    = _selection->contract( mIntervals, resultA, resultB );
        if( splitOccurred )
        {
            #ifdef ICP_MODULE_DEBUG_0
            #ifdef ICP_MODULE_DEBUG_1   
            cout << "Split occured: " << resultB << " and " << resultA << endl;
            #else
            cout << "Split occured" << endl;
            #endif
            #endif
            smtrat::icp::set_icpVariable variables;
            for( auto variableIt = _selection->constraint()->variables().begin(); variableIt != _selection->constraint()->variables().end(); ++variableIt )
            {
                assert(mVariables.find(*variableIt) != mVariables.end());
                variables.insert(mVariables.at(*variableIt));
            }
            mHistoryActual->addContraction(_selection, variables);
#ifdef BOXMANAGEMENT
            // set intervals and update historytree
            EvalDoubleIntervalMap tmpRight;
            for ( auto intervalIt = mIntervals.begin(); intervalIt != mIntervals.end(); ++intervalIt )
            {
                if ( (*intervalIt).first == variable )
                    tmpRight.insert(std::pair<const carl::Variable,smtrat::DoubleInterval>(variable, resultA ));
                else
                    tmpRight.insert((*intervalIt));
            }

            #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
            PointerSet<Formula> partialBox = createConstraintsFromBounds(tmpRight);
            Formula* negBox = new Formula(NOT);
            Formula* boxConjunction = new Formula(AND);
            for ( auto formulaIt = partialBox.begin(); formulaIt != partialBox.end(); ++formulaIt )
                boxConjunction->addSubformula(*formulaIt);
            
            negBox->addSubformula(boxConjunction);
            mCheckContraction->addSubformula(negBox);
            partialBox.clear();
            #endif

            icp::HistoryNode* newRightChild = new icp::HistoryNode(tmpRight, mCurrentId+2);
            newRightChild->setSplit( icp::intervalToConstraint( variable,tmpRight.at(variable) ).first );
            mHistoryActual->addRight(newRightChild);
            #ifdef ICP_MODULE_DEBUG_1
            cout << "Created node:" << endl;
            newRightChild->print();
            #endif
            
            // left first!
            EvalDoubleIntervalMap tmpLeft = EvalDoubleIntervalMap();
            for ( auto intervalIt = mIntervals.begin(); intervalIt != mIntervals.end(); ++intervalIt )
            {
                if ( (*intervalIt).first == variable )
                    tmpLeft.insert(std::pair<const carl::Variable,smtrat::DoubleInterval>(variable, resultB ));
                else
                    tmpLeft.insert((*intervalIt));
            }
            #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
            partialBox = createConstraintsFromBounds(tmpLeft);
            Formula* negBox2 = new Formula(NOT);
            Formula* boxConjunction2 = new Formula(AND);
            for ( auto formulaIt = partialBox.begin(); formulaIt != partialBox.end(); ++formulaIt )
                boxConjunction2->addSubformula(*formulaIt);
            
            negBox2->addSubformula(boxConjunction2);
            mCheckContraction->addSubformula(negBox2);
            addAssumptionToCheck(*mCheckContraction,false,"SplitCheck");
            mCheckContraction->clear();
            #endif
            icp::HistoryNode* newLeftChild = new icp::HistoryNode(tmpLeft,++mCurrentId);
            newLeftChild->setSplit( icp::intervalToConstraint( variable, tmpLeft.at(variable) ).second );
            ++mCurrentId;
            mHistoryActual = mHistoryActual->addLeft(newLeftChild);
            #ifdef ICP_MODULE_DEBUG_1   
            cout << "Created node:" << endl;
            newLeftChild->print();
            #endif
            // update mIntervals - usually this happens when changing to a different box, but in this case it has to be done manually, otherwise mIntervals is not affected.
            mIntervals[variable] = resultB;
#else
            /// create prequesites: ((oldBox AND CCs) -> newBox) in CNF: (oldBox OR CCs) OR newBox 
            PointerSet<Formula> subformulas;
            PointerSet<Formula> splitPremise = createPremiseDeductions();
            for( const Formula* subformula : splitPremise )
                subformulas.insert( newNegation( subformula ) );
            // construct new box
            PointerSet<Formula> boxFormulas = createBoxFormula();
            // push deduction
            if( boxFormulas.size() > 1 )
            {
                auto lastFormula = --boxFormulas.end();
                for( auto iter = boxFormulas.begin(); iter != lastFormula; ++iter )
                {
                    PointerSet<Formula> subformulasTmp = subformulas;
                    subformulasTmp.insert( *iter );
                    addDeduction( newFormula( OR, subformulas ) );
                }
            }

            // create split: (not h_b OR (Not x<b AND x>=b) OR (x<b AND Not x>=b) )
            assert(resultA.upperBoundType() != BoundType::INFTY );
            Rational bound = carl::rationalize<Rational>( resultA.upper() );
            Module::branchAt( Polynomial( variable ), bound, splitPremise, true );
            cout << "division causes split on " << variable << " at " << bound << "!" << endl << endl;
#endif
            // TODO: Shouldn't it be the average of both contractions?
            _relativeContraction = (originalDiameter - resultB.diameter()) / originalInterval.diameter();
            _absoluteContraction = originalDiameter - resultB.diameter();
        }
        else
        {
            // set intervals
            mIntervals[variable] = resultA;
            #ifdef ICP_MODULE_DEBUG_0
            cout << "      New interval: " << variable << " = " << mIntervals.at(variable) << endl;
            #endif
            if ( mIntervals.at(variable).upperBoundType() != carl::BoundType::INFTY && mIntervals.at(variable).lowerBoundType() != carl::BoundType::INFTY && !originalUnbounded )
            {
                if ( originalDiameter == 0 )
                {
                    _relativeContraction = 0;
                    _absoluteContraction = 0;
                }
                else
                {
                    _relativeContraction = 1 - (mIntervals.at(variable).diameter() / originalDiameter);
                    _absoluteContraction = originalDiameter - mIntervals.at(variable).diameter();
                }
            }
            else if ( originalUnbounded && mIntervals.at(variable).isUnbounded() == false ) // if we came from infinity and got a result, we achieve maximal relative contraction
            {
                _relativeContraction = 1;
                _absoluteContraction = std::numeric_limits<double>::infinity();
            }
            
            if (_relativeContraction > 0)
            {
                mHistoryActual->addInterval(_selection->lhs(), mIntervals.at(_selection->lhs()));
                smtrat::icp::set_icpVariable variables;
                for( auto variableIt = _selection->constraint()->variables().begin(); variableIt != _selection->constraint()->variables().end(); ++variableIt )
                {
                    assert(mVariables.find(*variableIt) != mVariables.end());
                    variables.insert(mVariables.at(*variableIt));
                }
                mHistoryActual->addContraction(_selection, variables);
            }
            
            #ifdef ICP_MODULE_DEBUG_0
            cout << "      Relative contraction: " << _relativeContraction << endl;
            #endif
        }
        return splitOccurred;
    }
    
    std::map<carl::Variable, double> ICPModule::createModel( bool antipoint ) const
    {
        // Note that we do not need to consider INFTY bounds in the calculation of the antipoint.
        std::map<carl::Variable, double> assignments;
        for( auto varIt = mVariables.begin(); varIt != mVariables.end(); ++varIt )
        {
            double value;
            switch( (*varIt).second->isInternalUpdated() )
            {
                case icp::Updated::BOTH:
                    if(antipoint)
                        value = mIntervals.at((*varIt).second->var()).lower();
                    else
                        value = mIntervals.at((*varIt).second->var()).sample();
                    break;
                case icp::Updated::LEFT:
                    if(antipoint)
                        value = mIntervals.at((*varIt).second->var()).lower();
                    else 
                    {
                        if (mIntervals.at((*varIt).second->var()).upperBoundType() == BoundType::INFTY)
                            value = std::ceil(mIntervals.at((*varIt).second->var()).lower());
                        else
                            value = mIntervals.at((*varIt).second->var()).upper();
                    }
                    break;
                case icp::Updated::RIGHT:
                    if(antipoint)
                        value = mIntervals.at((*varIt).second->var()).upper();
                    else
                    {
                        if (mIntervals.at((*varIt).second->var()).lowerBoundType() == BoundType::INFTY)
                            value = std::floor(mIntervals.at((*varIt).second->var()).upper());
                        else
                            value = mIntervals.at((*varIt).second->var()).lower();
                    }
                    break;
                case icp::Updated::NONE:
                    if(antipoint)
                        value = mIntervals.at((*varIt).second->var()).sample();
                    else
                    {
                        if (mIntervals.at((*varIt).second->var()).lowerBoundType() == BoundType::INFTY)
                            value = std::floor(mIntervals.at((*varIt).second->var()).upper());
                        else
                            value = mIntervals.at((*varIt).second->var()).lower();
                    }
                    break;
                default:
                    break;
            }
            assignments.insert( std::make_pair((*varIt).second->var(), value) );
        }
        return assignments;
    }
    
    void ICPModule::updateModel() const
    {
        clearModel();
        if( solverState() == True )
        {
            if( mFoundSolution.empty() )
            {
                Module::getBackendsModel();
                EvalRationalMap rationalAssignment = mLRA.getRationalModel();
                for( auto assignmentIt = rationalAssignment.begin(); assignmentIt != rationalAssignment.end(); ++assignmentIt )
                {
                    auto varIt = mVariables.find((*assignmentIt).first);
                    if(  varIt != mVariables.end() && (*varIt).second->isOriginal() )
                    {
                        Polynomial value = Polynomial( assignmentIt->second );
                        Assignment assignment = vs::SqrtEx(value);
                        mModel.insert(std::make_pair(assignmentIt->first, assignment));
                    }
                }
            }
            else
            {   
                for( auto assignmentIt = mFoundSolution.begin(); assignmentIt != mFoundSolution.end(); ++assignmentIt )
                {
                    auto varIt = mVariables.find((*assignmentIt).first);
                    if(  varIt != mVariables.end() && (*varIt).second->isOriginal() )
                    {
                        Polynomial value = Polynomial( assignmentIt->second );
                        Assignment assignment = vs::SqrtEx(value);
                        mModel.insert( std::make_pair( assignmentIt->first, assignment ) );
                    }
                }
            }
        }
    }
    
    void ICPModule::tryContraction( icp::ContractionCandidate* _selection, double& _relativeContraction, const EvalDoubleIntervalMap& _intervals )
    {
        EvalDoubleIntervalMap intervals = _intervals;
        smtrat::DoubleInterval resultA = smtrat::DoubleInterval();
        smtrat::DoubleInterval resultB = smtrat::DoubleInterval();
        bool splitOccurred = false;

        // check if derivative is already calculated
        if(_selection->derivative() == 0)
            _selection->calcDerivative();

        const Polynomial               constr     = _selection->rhs();
        const Polynomial               derivative = _selection->derivative();
        const carl::Variable           variable   = _selection->derivationVar();
        assert(intervals.find(variable) != intervals.end());
        double                 originalDiameter = intervals.at(variable).diameter();
        bool originalUnbounded = ( intervals.at(variable).lowerBoundType() == carl::BoundType::INFTY || intervals.at(variable).upperBoundType() == carl::BoundType::INFTY );
        
//        splitOccurred = mIcp.contract<GiNaCRA::SimpleNewton>( intervals, constr, derivative, variable, resultA, resultB );
        splitOccurred    = _selection->contract( mIntervals, resultA, resultB );
        
        if( splitOccurred )
        {
            smtrat::DoubleInterval originalInterval = intervals.at(variable);
            
            EvalDoubleIntervalMap tmpRight = EvalDoubleIntervalMap();
            for ( auto intervalIt = intervals.begin(); intervalIt != intervals.end(); ++intervalIt )
            {
                if ( (*intervalIt).first == variable )
                    tmpRight.insert(std::pair<const carl::Variable,smtrat::DoubleInterval>(variable, resultA ));
                else
                    tmpRight.insert((*intervalIt));
            }
            
            // left first!
            EvalDoubleIntervalMap tmpLeft = EvalDoubleIntervalMap();
            for ( auto intervalIt = intervals.begin(); intervalIt != intervals.end(); ++intervalIt )
            {
                if ( (*intervalIt).first == variable )
                    tmpLeft.insert(std::pair<const carl::Variable,smtrat::DoubleInterval>(variable, resultB ));
                else
                    tmpLeft.insert((*intervalIt));
            }
            _relativeContraction = (originalDiameter - resultB.diameter()) / originalInterval.diameter();
        }
        else
        {
            // set intervals
            intervals[variable] = resultA;
            if ( intervals.at(variable).upperBoundType() != carl::BoundType::INFTY && intervals.at(variable).lowerBoundType() != carl::BoundType::INFTY && !originalUnbounded )
            {
                if ( originalDiameter == 0 )
                    _relativeContraction = 0;
                else
                    _relativeContraction = 1 - (intervals.at(variable).diameter() / originalDiameter);
            }
            else if ( originalUnbounded && intervals.at(variable).isUnbounded() == false ) // if we came from infinity and got a result, we achieve maximal relative contraction
                _relativeContraction = 1;
        }
    }
    
    double ICPModule::calculateSplittingImpact ( const carl::Variable& _var, icp::ContractionCandidate& _candidate ) const
    {
        double impact = 0;
        assert(mIntervals.count(_var) > 0);
        //assert(_var == _candidate.derivationVar()); // must be uncommented in order to be compilable with clang++
        double originalDiameter = mIntervals.at(_var).diameter();
        switch(mSplittingStrategy)
        {
            case 1: // Select biggest interval
            {
                impact = originalDiameter;
                break;
            }
            case 2: // Rule of Hansen and Walster - select interval with most varying function values
            {
                EvalDoubleIntervalMap* tmpIntervals = new EvalDoubleIntervalMap(mIntervals);
                tmpIntervals->insert(std::make_pair(_var,smtrat::DoubleInterval(1)));
                smtrat::DoubleInterval derivedEvalInterval = carl::IntervalEvaluation::evaluate(_candidate.derivative(), *tmpIntervals);
                impact = derivedEvalInterval.diameter() * originalDiameter;
                delete tmpIntervals;
                break;
            }
            case 3: // Rule of Ratz - minimize width of inclusion
            {
                EvalDoubleIntervalMap* tmpIntervals = new EvalDoubleIntervalMap(mIntervals);
                tmpIntervals->insert(std::make_pair(_var,smtrat::DoubleInterval(1)));
                smtrat::DoubleInterval derivedEvalInterval = carl::IntervalEvaluation::evaluate(_candidate.derivative(), *tmpIntervals);
                smtrat::DoubleInterval negCenter = smtrat::DoubleInterval(mIntervals.at(_var).sample()).inverse();
                negCenter = negCenter.add(mIntervals.at(_var));
                derivedEvalInterval = derivedEvalInterval.mul(negCenter);
                impact = derivedEvalInterval.diameter();
                delete tmpIntervals;
                break;
            }
            case 4: // Select according to optimal machine representation of bounds
            {
                if(mIntervals.at(_var).contains(0))
                {
                    impact = originalDiameter;
                }
                else
                {
                    impact = originalDiameter/(mIntervals.at(_var).upper() > 0 ? mIntervals.at(_var).lower() : mIntervals.at(_var).upper());
                }
                break;
            }
            default:
            {
                impact = originalDiameter;
                break;
            }
        }
        #ifdef ICP_MODULE_DEBUG_0
        cout << __PRETTY_FUNCTION__ << " Rule " << mSplittingStrategy << ": " << impact << endl;
        #endif
        return impact;
    }

    PointerSet<Formula> ICPModule::createPremiseDeductions()
    {
        // collect applied contractions
        PointerSet<Formula> contractions = mHistoryActual->appliedConstraints();
        // collect original box
//        assert( mBoxStorage.size() == 1 );
        PointerSet<Formula> box = mBoxStorage.front();
        contractions.insert( box.begin(), box.end() );
        mBoxStorage.pop();
        return contractions;
    }
    
    PointerSet<Formula> ICPModule::createBoxFormula()
    {
        Variables originalRealVariables;
        mpReceivedFormula->realValuedVars(originalRealVariables);
        PointerSet<Formula> subformulas;
        for( auto intervalIt = mIntervals.begin(); intervalIt != mIntervals.end(); ++intervalIt )
        {
            if( originalRealVariables.find( (*intervalIt).first ) != originalRealVariables.end() )
            {
                std::pair<const Constraint*, const Constraint*> boundaries = icp::intervalToConstraint((*intervalIt).first, (*intervalIt).second);
                if(boundaries.first != NULL)
                {
                    subformulas.insert( newFormula( boundaries.first ) );                       
                }
                if(boundaries.second != NULL)
                {
                    subformulas.insert( newFormula( boundaries.second ) );
                }
            }
        }
        return subformulas;
    }
    
    std::pair<bool,carl::Variable> ICPModule::checkAndPerformSplit( )
    {
        std::pair<bool,carl::Variable> result = std::make_pair(false, carl::Variable::NO_VARIABLE);
        bool found = false;
        carl::Variable variable = carl::Variable::NO_VARIABLE; // Initialized to some dummy value
        double maximalImpact = 0;   
        // first check all intervals from nonlinear contractionCandidates -> backwards to begin at the most important candidate
        for( auto candidateIt = mActiveNonlinearConstraints.rbegin(); candidateIt != mActiveNonlinearConstraints.rend(); ++candidateIt )
        {
            if( found )
                break;
            if( (*candidateIt)->isActive() )
            {
                variable = *(*candidateIt)->constraint()->variables().begin();
                // search for the biggest interval and check if it is larger than the target Diameter
                for ( auto variableIt = (*candidateIt)->constraint()->variables().begin(); variableIt != (*candidateIt)->constraint()->variables().end(); ++variableIt )
                {
                    std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.find(*variableIt);
                    assert(icpVar != mVariables.end());
                    if ( mIntervals.find(*variableIt) != mIntervals.end() && mIntervals.at(*variableIt).diameter() > mTargetDiameter && (*icpVar).second->isOriginal() )
                    {
                        if(mSplittingStrategy > 0)
                        {
                            double actualImpact = calculateSplittingImpact(*variableIt, **candidateIt);
                            if( actualImpact > maximalImpact )
                            {
                                variable = *variableIt;
                                found = true;
                                maximalImpact = actualImpact;
                            }
                        }
                        else
                        {
                            variable = *variableIt;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        for ( auto candidateIt = mActiveLinearConstraints.rbegin(); candidateIt != mActiveLinearConstraints.rend(); ++candidateIt )
        {
            if( found )
                break;
            if( (*candidateIt)->isActive() )
            {
                variable = *(*candidateIt)->constraint()->variables().begin();
                // search for the biggest interval and check if it is larger than the target Diameter
                for( auto variableIt = (*candidateIt)->constraint()->variables().begin(); variableIt != (*candidateIt)->constraint()->variables().end(); ++variableIt )
                {
                    std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.find(*variableIt);
                    assert(icpVar != mVariables.end());
                    if ( mIntervals.find(*variableIt) != mIntervals.end() && mIntervals.at(*variableIt).diameter() > mTargetDiameter && (*icpVar).second->isOriginal() )
                    {
                        if(mSplittingStrategy > 0)
                        {
                            double actualImpact = calculateSplittingImpact(*variableIt, **candidateIt);
                            if( actualImpact > maximalImpact )
                            {
                                variable = *variableIt;
                                found = true;
                                maximalImpact = actualImpact;
                            }
                        }
                        else
                        {
                            variable = *variableIt;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if( found )
        {
            #ifndef BOXMANAGEMENT
            // create prequesites: ((oldBox AND CCs) -> newBox) in CNF: (oldBox OR CCs) OR newBox 
            PointerSet<Formula> splitPremise = createPremiseDeductions();
            PointerSet<Formula> subformulas;
            for( auto formulaIt = splitPremise.begin(); formulaIt != splitPremise.end(); ++formulaIt )
                subformulas.insert( newNegation( *formulaIt ) );
            // construct new box
            subformulas.insert( newFormula( AND, std::move( createBoxFormula() ) ) );
            // push deduction
            addDeduction( newFormula( OR, subformulas ) );
            
            // create split: (not h_b OR (Not x<b AND x>=b) OR (x<b AND Not x>=b) )
            Rational bound = carl::rationalize<Rational>( mIntervals.at(variable).sample( false ) );
            Module::branchAt( Polynomial( variable ), bound, splitPremise, false );
            cout << "force split on " << variable << " at " << bound << "!" << endl << endl;
            
            result.first = true;
            result.second = variable;
            return result;
            #else
            //perform split and add two historyNodes
            #ifdef ICP_MODULE_DEBUG_0
            cout << "[ICP] Split performed in: " << variable<< endl;
            cout << "Size mIntervals: " << mIntervals.size() << endl;
            #endif
            // set intervals and update historytree
            DoubleInterval tmp = mIntervals.at(variable);
            DoubleInterval tmpRightInt = tmp;
            tmpRightInt.cutUntil(tmp.sample());
            tmpRightInt.setLeftType(BoundType::WEAK);
            mIntervals[variable] = tmpRightInt;
            EvalDoubleIntervalMap tmpRight;

            for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
                tmpRight.insert((*constraintIt));

            icp::HistoryNode* newRightChild = new icp::HistoryNode(tmpRight, mCurrentId+2);
            std::pair<const Constraint*, const Constraint*> boundaryConstraints = icp::intervalToConstraint(variable, tmpRightInt);
            newRightChild->setSplit(boundaryConstraints.first);
            mHistoryActual->addRight(newRightChild);

            // left first!
            DoubleInterval tmpLeftInt = tmp;
            tmpLeftInt.cutFrom(tmp.sample());
            tmpLeftInt.setRightType(BoundType::STRICT);
            mIntervals[variable] = tmpLeftInt;
            EvalDoubleIntervalMap tmpLeft;

            for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
                tmpLeft.insert((*constraintIt));

            icp::HistoryNode* newLeftChild = new icp::HistoryNode(tmpLeft, ++mCurrentId);
            boundaryConstraints = icp::intervalToConstraint(variable, tmpLeftInt);
            newLeftChild->setSplit(boundaryConstraints.second);
            ++mCurrentId;
            mHistoryActual = mHistoryActual->addLeft(newLeftChild);
            updateRelevantCandidates(variable, 0.5 );
            // only perform one split at a time and then contract
            result.first = true;
            result.second = variable;
            std::map<string, icp::IcpVariable*>::iterator icpVar = mVariables.find(variable.get_name());
            assert(icpVar != mVariables.end());
            (*icpVar).second->setUpdated();
            return result;
            #endif
        }
        return result;
    }
    
    bool ICPModule::tryTestPoints()
    {
        bool testSuccessful = true;
        // validate the antipoint
        std::map<carl::Variable, double> antipoint = createModel( true );
        mFoundSolution.clear();
        #ifdef ICP_MODULE_DEBUG_0
        cout << "Try test point:" << endl;
        #endif
        for( auto iter = antipoint.begin(); iter != antipoint.end(); ++iter )
        {
            #ifdef ICP_MODULE_DEBUG_0
            cout << "    " << iter->first << " -> " << carl::rationalize<Rational>( iter->second ) << endl;
            #endif
            mFoundSolution.insert( std::make_pair( iter->first, carl::rationalize<Rational>( iter->second ) ) );
        }
        ContractionCandidates candidates;
//        for( auto iter = mLinearConstraints.begin(); iter != mLinearConstraints.end(); ++iter )
//        {
//            assert( !iter->second.empty() );
//            if( iter->first->isSatisfiedBy( mFoundSolution[iter->second.begin()->lhs()] ) )
//            {
//                candidates.insert( iter->second.begin(), iter->second.end() );
//            }
//        }
//        for( auto candidate = mActiveNonlinearConstraints.begin(); candidate != mActiveNonlinearConstraints.end(); ++candidate )
//        {
//            unsigned isSatisfied = (*candidate)->constraint()->satisfiedBy( mFoundSolution );
//            assert( isSatisfied != 2 );
//            if( isSatisfied == 0 )
//            {
//                testSuccessful = false;
//            }
//        }
        // if a change has happened we need to restart at the latest point possible
        if( !candidates.empty() )
        {
            testSuccessful = false;
            for( auto cand : candidates )
            {
                addCandidateToRelevant( cand );
            }
            mHistoryActual->propagateStateInfeasibleConstraints();
            mHistoryActual->propagateStateInfeasibleVariables();
            setBox( mHistoryRoot );
            mHistoryActual = mHistoryActual->addRight( new icp::HistoryNode( mHistoryRoot->intervals(), 2 ) );
            mCurrentId = mHistoryActual->id();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Test point failed!" << endl;
            #endif
        }
        if( !testSuccessful )
            mFoundSolution.clear();
        // auto-activate all ICP-variables
        for( auto varIt = mVariables.begin(); varIt != mVariables.end(); ++varIt )
            (*varIt).second->autoActivate();
        return testSuccessful;
    }

//    bool ICPModule::validateSolution( bool& _newConstraintAdded )
//    {
//        vec_set_const_pFormula failedConstraints;
//        PointerSet<Formula> currentInfSet;
//        _newConstraintAdded = false;
//        #ifdef ICP_MODULE_DEBUG_0
//        cout << "Validate solution:" << endl;
//        cout << "[ICP] Call mLRAModule" << endl;
//        #endif
//        #ifdef ICP_SIMPLE_VALIDATION
//        // validate the antipoint
//        std::map<carl::Variable, double> antipoint = createModel( true );
//        EvalDoubleIntervalMap tmp;
//        for( auto iter = antipoint.begin(); iter != antipoint.end(); ++iter )
//            tmp.insert( std::make_pair( iter->first, DoubleInterval( iter->second ) ) );
//        ContractionCandidates candidates;
//        for( auto candidate = mActiveLinearConstraints.begin(); candidate != mActiveLinearConstraints.end(); ++candidate )
//        {
//            const Constraint* constraint = (*candidate)->constraint();
//            unsigned isSatisfied = constraint->consistentWith( tmp );
//            if( isSatisfied == 0 )
//            {
//                if( !(*candidate)->isActive() )
//                {
//                    candidates.insert((*candidate).first);
//                    (*candidate)->activate();
//                    _newConstraintAdded = true;
//                }
//                
//            }
//        }
//        // if a change has happened we need to restart at the latest point possible
//        if( _newConstraintAdded )
//        {
//            mHistoryActual->propagateStateInfeasibleConstraints();
//            mHistoryActual->propagateStateInfeasibleVariables();
//            icp::HistoryNode* found = tryToAddConstraint( candidates, mHistoryRoot->right() );
//            if( found == NULL )
//            {
//                setBox( mHistoryRoot );
//                mHistoryActual = mHistoryActual->addRight( new icp::HistoryNode( mHistoryRoot->intervals(), 2 ) );
//                mCurrentId = mHistoryActual->id();
//            }
//            else
//                setBox( found );
//        }
//        // autoactivate all icpVariables
//        for( auto varIt = mVariables.begin(); varIt != mVariables.end(); ++varIt )
//            (*varIt).second->autoActivate();
//        return true;
//        #else
//        // create new center constraints and add to validationFormula
//        for ( auto variableIt = mVariables.begin(); variableIt != mVariables.end(); ++variableIt)
//        {
//            if ( (*variableIt).second->checkLinear() == false )
//            {
//                carl::Variable variable = (*variableIt).second->var();
//                assert(mIntervals.find(variable) != mIntervals.end());
//                smtrat::DoubleInterval interval = mIntervals.at(variable);
//
//                smtrat::DoubleInterval center = smtrat::DoubleInterval(interval.sample());
//                Polynomial constraint = Polynomial(variable) - Polynomial(carl::rationalize<Rational>(center.sample()));
//                const Formula* centerTmpFormula = newFormula( newConstraint( constraint, Relation::EQ ) );
//                mLRA.inform(centerTmpFormula->pConstraint());
//                mCenterConstraints.insert(centerTmpFormula->pConstraint());
//                mValidationFormula->push_back( centerTmpFormula );
//            }
//        }
//        
//        // assert all constraints in mValidationFormula
//        // TODO: optimize! -> should be okay to just assert centerconstraints
//        for ( auto valIt = mValidationFormula->begin(); valIt != mValidationFormula->end(); ++valIt)
//            mLRA.assertSubformula(valIt);
//
//        #ifdef ICP_MODULE_DEBUG_0
//        cout << "[mLRA] receivedFormula: " << endl;
//        cout << mLRA.rReceivedFormula().toString() << endl;
//        #endif
//        mLRA.rReceivedFormula().updateProperties();
//        Answer centerFeasible = mLRA.isConsistent();
//        mLRA.clearDeductions();
//        
//        if ( centerFeasible == True )
//        {
//            // remove centerConstaints as soon as they are not longer needed.
//            clearCenterConstraintsFromValidationFormula();
//            // strong consistency check
//            EvalRationalMap pointsolution = mLRA.getRationalModel();
//            #ifdef ICP_MODULE_DEBUG_0
//            cout << "[mLRA] Pointsolution: " << pointsolution << endl;
//            #endif
//            /*
//             * fill linear variables with pointsolution b, determine coefficients c
//             * of nonlinear variables x, take lower or upper bound correspondingly.
//             * For every active linear constraint:
//             *          check:
//             *          c*x <= e + d*b
//             * e = constant part,
//             * d = coefficient of linear variable
//             */
//
//            // For every active linear constraint:
//            for ( auto linearIt = mActiveLinearConstraints.begin(); linearIt != mActiveLinearConstraints.end(); ++linearIt)
//            {
//                Polynomial constraint = (*linearIt)->rhs();
//                Polynomial nonlinearParts;
//                Rational res = 0;
//                bool isLeftInfty = false;
//                bool isRightInfty = false;
//                bool satisfied = false;
//                
//                constraint += (*linearIt)->lhs();
//                constraint = constraint.substitute(pointsolution);
//                
//                std::map<carl::Variable, Rational> nonlinearValues;
//                
//                for( auto term = constraint.begin(); term != constraint.end(); ++term)
//                {
//                    Variables vars;
//                    if(!(*term)->monomial())
//                    {
//                        continue; // Todo: sure?
//                    }
//                    else
//                    {
//                        (*term)->monomial()->gatherVariables(vars);
//                        if( (*term)->coeff() < 0 )
//                        {
//                            for(auto varIt = vars.begin(); varIt != vars.end(); ++varIt)
//                            {
//                                if(mIntervals.at(*varIt).lowerBoundType() != BoundType::INFTY)
//                                    nonlinearValues.insert(std::make_pair(*varIt, carl::rationalize<Rational>(mIntervals.at(*varIt).lower())) );
//                                else
//                                    isLeftInfty = true;
//                            }
//                        }
//                        else
//                        {
//                            for(auto varIt = vars.begin(); varIt != vars.end(); ++varIt)
//                            {
//                                if(mIntervals.at(*varIt).upperBoundType() != BoundType::INFTY) 
//                                    nonlinearValues.insert(std::make_pair(*varIt, carl::rationalize<Rational>(mIntervals.at(*varIt).upper())) );
//                                else
//                                    isRightInfty = true;
//                            }
//                        }
//                        if( !(isLeftInfty || isRightInfty) )
//                        {  
//                            carl::Term<Rational>* tmp = (*term)->monomial()->substitute(nonlinearValues, (*term)->coeff());
//                            assert(tmp->isConstant());
//                            nonlinearParts += tmp->coeff();
//                        }
//                        nonlinearValues.clear();
//                    }
//                }
//                Rational val = 0;
//                if(constraint.isConstant())
//                {
//                    constraint += nonlinearParts;
//                    val = constraint.isZero() ? 0 : constraint.lcoeff();
//                }
//                
//                switch ((*linearIt)->constraint()->relation())
//                {
//                    case Relation::EQ: //CR_EQ = 0
//                        satisfied = (val == 0 && !isLeftInfty && !isRightInfty);
//                        break;
//                    case Relation::NEQ: //CR_NEQ = 1
//                        satisfied = (val != 0 || isLeftInfty || isRightInfty);
//                        break;
//                    case Relation::LESS: //CR_LESS = 2
//                        satisfied = (val < 0 || isLeftInfty);
//                        break;
//                    case Relation::GREATER: //CR_GREATER = 3
//                        satisfied = (val > 0 || isRightInfty);
//                        break;
//                    case Relation::LEQ: //CR_LEQ = 4
//                        satisfied = (val <= 0 || isLeftInfty);
//                        break;
//                    case Relation::GEQ: //CR_GEQ = 5
//                        satisfied = (val >= 0 || isRightInfty);
//                        break;
//                }
//                #ifdef ICP_MODULE_DEBUG_1
//                cout << "[ICP] Validate: " << *linearIt->first->constraint() << " -> " << satisfied << " (" << constraint << ") " << endl;
//                cout << "Candidate: ";
//                linearIt->first->print();
//                #endif
//                // Strong consistency check
//                if ( !satisfied )
//                {
//                    // parse mValidationFormula to get pointer to formula to generate infeasible subset
//                    for ( auto formulaIt = mValidationFormula->begin(); formulaIt != mValidationFormula->end(); ++formulaIt )
//                    {
//                        for( auto originIt = (*linearIt)->rOrigin().begin(); originIt != (*linearIt)->rOrigin().end(); ++originIt )
//                        {
//                            if ((*formulaIt)->pConstraint() == (*originIt)->pConstraint() )
//                            {
//                                currentInfSet.insert(*formulaIt);
//                                break;
//                            }
//                        }
//                    }
//                }
//
//            } // for every linear constraint
//            
//            if ( !currentInfSet.empty() )
//                failedConstraints.push_back(currentInfSet);
//           
//            _newConstraintAdded = updateIcpRelevantCandidates( failedConstraints );
//            return true;
//        }
//        else
//        {
//            assert( centerFeasible == False );
//            _newConstraintAdded = updateIcpRelevantCandidates( mLRA.infeasibleSubsets() );
//            clearCenterConstraintsFromValidationFormula();
//            #ifdef ICP_MODULE_DEBUG_0
//            if( _newConstraintAdded )
//                cout << "New ICP-relevant contraction candidates added!" << endl; 
//            cout << "Validation failed!" << endl;
//            #endif
//            return false;
//        }
//        #endif
//    }
//    
//    bool ICPModule::updateIcpRelevantCandidates( const vec_set_const_pFormula& _infSubsetsInLinearization )
//    {
//        bool newConstraintAdded = false;
//        ContractionCandidates candidates;
//        // TODO: Das muss effizienter gehen! CORRECT?
//        for ( auto vecIt = _infSubsetsInLinearization.begin(); vecIt != _infSubsetsInLinearization.end(); ++vecIt )
//        {
//            for ( auto infSetIt = (*vecIt).begin(); infSetIt != (*vecIt).end(); ++infSetIt )
//            {
//                // if the failed constraint is not a centerConstraint - Ignore centerConstraints
//                if ( mCenterConstraints.find((*infSetIt)->pConstraint()) == mCenterConstraints.end() )
//                {
//                    // add candidates for all variables to icpRelevantConstraints  
//                    auto iterB = mDeLinearizations.find( *infSetIt );
//                    if ( iterB != mDeLinearizations.end() )
//                    {
//                        // search for the candidates and add them as icpRelevant
//                        for ( icp::ContractionCandidate* actCandidateIt : mActiveLinearConstraints )
//                        {
//                            if ( actCandidateIt->hasOrigin( iterB->second ) )
//                            {
//                                #ifdef ICP_MODULE_DEBUG_1                               
//                                cout << "isActive ";
//                                actCandidateIt->print();
//                                cout <<  " : " << actCandidateIt->isActive() << endl;
//                                #endif
//
//                                // if the candidate is not active we really added a constraint -> indicate the change
//                                if ( !actCandidateIt->isActive() )
//                                {
//                                    actCandidateIt->activate();
//                                    candidates.insert( actCandidateIt );
//                                    newConstraintAdded = true;
//                                }
//
//                                // activate all icpVariables for that candidate
//                                for ( auto variableIt = actCandidateIt->constraint()->variables().begin(); variableIt != actCandidateIt->constraint()->variables().end(); ++variableIt )
//                                {
//                                    std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.find(*variableIt);
//                                    assert(icpVar != mVariables.end());
//                                    (*icpVar).second->activate();
//                                }
//                            } // found correct linear replacement
//                        } // iterate over active linear constraints
//                    } // is a linearization replacement
//                    else
//                    {
//                        // this should not happen
//                        assert(false);
//                    }
//                } // is no center constraint
//            }
//        }
//            
//        if(newConstraintAdded)
//        {
//            mHistoryActual->propagateStateInfeasibleConstraints();
//            mHistoryActual->propagateStateInfeasibleVariables();
//            icp::HistoryNode* found = tryToAddConstraint(candidates, mHistoryRoot->right());
//            if(found == NULL)
//            {
//                setBox(mHistoryRoot);
//                mHistoryActual = mHistoryActual->addRight(new icp::HistoryNode(mHistoryRoot->intervals(),2));
//                mCurrentId = mHistoryActual->id();
//                assert( mCurrentId == 2);
//            }
//            else
//                setBox(found);
//        }
//        return newConstraintAdded;
//    }
    
    void ICPModule::clearCenterConstraintsFromValidationFormula()
    {
        for ( auto centerIt = mValidationFormula->begin(); centerIt != mValidationFormula->end(); )
        {
            if ( mCenterConstraints.find((*centerIt)->pConstraint()) != mCenterConstraints.end() )
            {
                mLRA.removeSubformula(centerIt);
                centerIt = mValidationFormula->erase(centerIt);
            }
            else
                ++centerIt;
        }
        mCenterConstraints.clear();
    }
    
    bool ICPModule::checkBoxAgainstLinearFeasibleRegion()
    {
        PointerSet<Formula> addedBoundaries = createConstraintsFromBounds(mIntervals);
        for( auto formulaIt = addedBoundaries.begin(); formulaIt != addedBoundaries.end(); ++formulaIt )
        {
            mLRA.inform((*formulaIt)->pConstraint());
            mValidationFormula->push_back( *formulaIt );
            mLRA.assertSubformula( --mValidationFormula->end() );
        }
        mLRA.rReceivedFormula().updateProperties();
        Answer boxCheck = mLRA.isConsistent();
        #ifdef ICP_MODULE_DEBUG_0
        cout << "Boxcheck: " << boxCheck << endl;
        #endif
        #ifdef SMTRAT_DEVOPTION_VALIDATION_ICP
        if ( boxCheck == False )
        {
            Formula* actualAssumptions = new Formula(*mValidationFormula);
            Module::addAssumptionToCheck(*actualAssumptions,false,"ICP_BoxValidation");
        }
        #endif
        if( boxCheck != True )
        {
            vec_set_const_pFormula tmpSet = mLRA.infeasibleSubsets();
            for ( auto infSetIt = tmpSet.begin(); infSetIt != tmpSet.end(); ++infSetIt )
            {
                for ( auto formulaIt = (*infSetIt).begin(); formulaIt != (*infSetIt).end(); ++formulaIt )
                {
                    if( !(*formulaIt)->pConstraint()->isBound() )
                    {
                        mHistoryActual->addInfeasibleConstraint((*formulaIt)->pConstraint());
                        for( auto variableIt = (*formulaIt)->constraint().variables().begin(); variableIt != (*formulaIt)->constraint().variables().end(); ++variableIt )
                        {
                            assert( mVariables.find(*variableIt) != mVariables.end() );
                            mHistoryActual->addInfeasibleVariable(mVariables.at(*variableIt));
                        }
                    }
                    else
                    {
                        assert( mVariables.find( *(*formulaIt)->pConstraint()->variables().begin() ) != mVariables.end() );
                        mHistoryActual->addInfeasibleVariable( mVariables.at( *(*formulaIt)->pConstraint()->variables().begin()) );
                    }
                }
            }
        }
        #ifdef ICP_PROLONG_CONTRACTION
        else
        {
            EvalIntervalMap bounds = mLRA.getVariableBounds();
            #ifdef ICP_MODULE_DEBUG_0
            cout << "Newly obtained Intervals: " << endl;
            #endif
            for ( auto boundIt = bounds.begin(); boundIt != bounds.end(); ++boundIt )
            {
                if (mVariables.find((*boundIt).first) != mVariables.end())
                {
                    Interval tmp = (*boundIt).second;
                    // mHistoryRoot->addInterval((*boundIt).first, smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType()) );
                    DoubleInterval newInterval = DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType() );
                    if( !(mIntervals.at((*boundIt).first) == newInterval) && mIntervals.at((*boundIt).first).contains(newInterval) )
                    {
                        #ifdef ICP_MODULE_DEBUG_0
                        cout << (*boundIt).first << ": " << (*boundIt).second << endl;
                        #endif
                        double relativeContraction = (mIntervals.at((*boundIt).first).diameter() - newInterval.diameter()) / mIntervals.at((*boundIt).first).diameter();
                        mIntervals[(*boundIt).first] = newInterval;
                        mVariables.at((*boundIt).first)->setUpdated();
                        updateRelevantCandidates((*boundIt).first, relativeContraction);
                    }
                }
            }
            
            // get intervals for slackvariables
            const LRAModule::ExVariableMap slackVariables = mLRA.slackVariables();
            for ( auto slackIt = slackVariables.begin(); slackIt != slackVariables.end(); ++slackIt )
            {
                std::map<const LRAVariable*, ContractionCandidates>::iterator linIt = mLinearConstraints.find((*slackIt).second);
                if ( linIt != mLinearConstraints.end() )
                {
                    // dirty hack: expect lhs to be set and take first item of set of CCs --> Todo: Check if it is really set in the constructors of the CCs during inform and assert
                    Interval tmp = (*slackIt).second->getVariableBounds();
                    // keep root updated about the initial box.
                    // mHistoryRoot->rIntervals()[(*(*linIt).second.begin())->lhs()] = smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType());
                    DoubleInterval newInterval = DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType() );
                    Variable var = (*(*linIt).second.begin())->lhs();
                    if( !(mIntervals.at(var) == newInterval) && mIntervals.at(var).contains(newInterval) )
                    {
                        double relativeContraction = (mIntervals.at(var).diameter() - newInterval.diameter()) / mIntervals.at(var).diameter();
                        mIntervals[var] = smtrat::DoubleInterval(tmp.lower(), tmp.lowerBoundType(), tmp.upper(), tmp.upperBoundType());
                        mVariables.at(var)->setUpdated();
                        updateRelevantCandidates(var, relativeContraction);
                        #ifdef ICP_MODULE_DEBUG_1
                        cout << "Added interval (slackvariables): " << var << " " << tmp << endl;
                        #endif
                    }
                }
            }
        }
        #endif
        
        // remove boundaries from mLRA module after boxChecking.
        for( auto boundIt = addedBoundaries.begin(); boundIt != addedBoundaries.end(); )
        {
            for (auto formulaIt = mValidationFormula->begin(); formulaIt != mValidationFormula->end(); )
            {
                if( (*boundIt)->constraint() == (*formulaIt)->constraint() )
                {
                    mLRA.removeSubformula(formulaIt);
                    formulaIt = mValidationFormula->erase(formulaIt);
                    break;
                }
                else
                {
                    ++formulaIt;
                }
            }
            boundIt = addedBoundaries.erase(boundIt);
        }
        
        mLRA.clearDeductions();
        assert(addedBoundaries.empty());
        
        if ( boxCheck == True )
            return true;
        return false;
    }
    
    bool ICPModule::chooseBox()
    {
        mLastCandidate = NULL;
        icp::HistoryNode* newBox = chooseBox( mHistoryActual );
        if ( newBox != NULL )
        {
            setBox(newBox);
            return true;
        }
        else
        {
            // no new Box to select -> finished
            // TODO: If chooseBox worked properly, this wouldn't be necessary.
            mHistoryActual->propagateStateInfeasibleConstraints();
            mHistoryActual->propagateStateInfeasibleVariables();

            mInfeasibleSubsets.clear();
            mInfeasibleSubsets.push_back(collectReasons(mHistoryRoot));
            // printInfeasibleSubsets();
            return false;
        }
    }
    
    icp::HistoryNode* ICPModule::chooseBox( icp::HistoryNode* _basis )
    {
        if ( _basis->isLeft() )
        {
            // if spliting constraint or the constraint resulting from a contraction
            // of the splitting constraint is included in the infeasible subset
            // skip the right box and continue.
            const carl::Variable variable = _basis->variable();
            assert( mIntervals.find(variable) != mIntervals.end() );
            if ( _basis->stateInfeasibleConstraintsContainSplit() )
            {
                // set infeasible subset
                for( auto constraintIt = _basis->rStateInfeasibleConstraints().begin(); constraintIt != _basis->rStateInfeasibleConstraints().end(); ++constraintIt )
                    _basis->parent()->addInfeasibleConstraint(*constraintIt);
                
                for( auto variableIt = _basis->rStateInfeasibleVariables().begin(); variableIt != _basis->rStateInfeasibleVariables().end(); ++variableIt )
                    _basis->parent()->addInfeasibleVariable(*variableIt,true);
            }
            else
            {
                if ( _basis->parent() == NULL )
                {
                    // should not happen: Root is defined to be a right-child
                    assert(false);
                    return NULL;
                }
                else
                {
                    // skip the right box
                    // set infeasible subset
                    for( auto constraintIt = _basis->rStateInfeasibleConstraints().begin(); constraintIt != _basis->rStateInfeasibleConstraints().end(); ++constraintIt )
                        _basis->parent()->addInfeasibleConstraint(*constraintIt);
                    
                    for( auto variableIt = _basis->rStateInfeasibleVariables().begin(); variableIt != _basis->rStateInfeasibleVariables().end(); ++variableIt )
                        _basis->parent()->addInfeasibleVariable(*variableIt,true);
                    
                    chooseBox(_basis->parent());
                }
            }
            return _basis->parent()->right();
        }
        else // isRight
        {
            if ( _basis->parent() == mHistoryRoot )
            {
                // set infeasible subset
                for( auto constraintIt = _basis->rStateInfeasibleConstraints().begin(); constraintIt != _basis->rStateInfeasibleConstraints().end(); ++constraintIt )
                    _basis->parent()->addInfeasibleConstraint(*constraintIt);
                
                for( auto variableIt = _basis->rStateInfeasibleVariables().begin(); variableIt != _basis->rStateInfeasibleVariables().end(); ++variableIt )
                    _basis->parent()->addInfeasibleVariable(*variableIt,true);
                
                return NULL;
            }
            else // select next starting from parent
            {
                // set infeasible subset
                for( auto constraintIt = _basis->rStateInfeasibleConstraints().begin(); constraintIt != _basis->rStateInfeasibleConstraints().end(); ++constraintIt )
                    _basis->parent()->addInfeasibleConstraint(*constraintIt);
                
                for( auto variableIt = _basis->rStateInfeasibleVariables().begin(); variableIt != _basis->rStateInfeasibleVariables().end(); ++variableIt )
                    _basis->parent()->addInfeasibleVariable(*variableIt,true);
                
                return chooseBox( _basis->parent() );
            }
        }
    }

    void ICPModule::pushBoundsToPassedFormula()
    {
        Variables originalRealVariables;
        mpReceivedFormula->realValuedVars(originalRealVariables);
        for( std::map<const carl::Variable, icp::IcpVariable*>::iterator icpVar = mVariables.begin(); icpVar != mVariables.end(); ++icpVar )
        {
            const carl::Variable::Arg tmpSymbol = icpVar->first;
            if( icpVar->second->isOriginal() && originalRealVariables.find( tmpSymbol ) != originalRealVariables.end() )
            {
                if( (*icpVar).second->isExternalBoundsSet() == icp::Updated::BOTH || (*icpVar).second->isExternalUpdated() != icp::Updated::NONE )
                {
                    // generate both bounds, left first
                    if( (*icpVar).second->isExternalBoundsSet() == icp::Updated::NONE || (*icpVar).second->isExternalBoundsSet() == icp::Updated::RIGHT 
                        || (*icpVar).second->isExternalUpdated() == icp::Updated::LEFT || (*icpVar).second->isExternalUpdated() == icp::Updated::BOTH )
                    {
                        assert( mIntervals.find(tmpSymbol) != mIntervals.end() );
                        Rational bound = carl::rationalize<Rational>(mIntervals.at(tmpSymbol).lower() );
                        Polynomial leftEx = Polynomial(tmpSymbol) - Polynomial(bound);

                        const Constraint* leftTmp;
                        switch (mIntervals.at(tmpSymbol).lowerBoundType())
                        {
                            case carl::BoundType::STRICT:
                                leftTmp = newConstraint(leftEx, Relation::GREATER);
                                break;
                            case carl::BoundType::WEAK:
                                leftTmp = newConstraint(leftEx, Relation::GEQ);

                                break;
                            default:
                                leftTmp = NULL;
                        }
                        if ( leftTmp != NULL )
                        {
                            const Formula* leftBound = newFormula(leftTmp);
                            vec_set_const_pFormula origins;
                            PointerSet<Formula> emptyTmpSet;
                            origins.insert(origins.begin(), emptyTmpSet);

                            if( (*icpVar).second->isExternalBoundsSet() == icp::Updated::LEFT )
                                removeSubformulaFromPassedFormula((*icpVar).second->externalLeftBound());
                            addConstraintToInform(leftTmp);
                            addSubformulaToPassedFormula( leftBound, move( origins ) );
                            (*icpVar).second->setExternalLeftBound(--mpPassedFormula->end());
                        }
                    }
                    
                    if( (*icpVar).second->isExternalBoundsSet() == icp::Updated::NONE || (*icpVar).second->isExternalBoundsSet() == icp::Updated::LEFT
                        || (*icpVar).second->isExternalUpdated() == icp::Updated::RIGHT || (*icpVar).second->isExternalUpdated() == icp::Updated::BOTH )
                    {
                        // right:
                        Rational bound = carl::rationalize<Rational>(mIntervals.at(tmpSymbol).upper());
                        Polynomial rightEx = Polynomial(tmpSymbol) - Polynomial(bound);
                        const Constraint* rightTmp;
                        switch( mIntervals.at(tmpSymbol).upperBoundType() )
                        {
                            case carl::BoundType::STRICT:
                                rightTmp = newConstraint(rightEx, Relation::LESS);
                                break;
                            case carl::BoundType::WEAK:
                                rightTmp = newConstraint(rightEx, Relation::LEQ);
                                break;
                            default:
                                rightTmp = NULL;
                        }
                        if( rightTmp != NULL )
                        {
                            const Formula* rightBound = newFormula(rightTmp);
                            vec_set_const_pFormula origins;
                            PointerSet<Formula> emptyTmpSet;
                            origins.insert(origins.begin(), emptyTmpSet);

                            if ( (*icpVar).second->isExternalBoundsSet() == icp::Updated::RIGHT )
                                removeSubformulaFromPassedFormula((*icpVar).second->externalRightBound());

                            addConstraintToInform(rightTmp);
                            addSubformulaToPassedFormula( rightBound, move( origins ) );
                            (*icpVar).second->setExternalRightBound(--mpPassedFormula->end());
                        }
                    }
                }
            }
        }
    }
    
    PointerSet<Formula> ICPModule::variableReasonHull( icp::set_icpVariable& _reasons )
    {
        PointerSet<Formula> reasons;
        for( auto variableIt = _reasons.begin(); variableIt != _reasons.end(); ++variableIt )
        {
            if ((*variableIt)->lraVar() != NULL)
            {
                PointerSet<Formula> definingOrigins = (*variableIt)->lraVar()->getDefiningOrigins();
                for( auto formulaIt = definingOrigins.begin(); formulaIt != definingOrigins.end(); ++formulaIt )
                {
                    // cout << "Defining origin: " << **formulaIt << " FOR " << *(*variableIt) << endl;
                    bool hasAdditionalVariables = false;
                    Variables realValuedVars;
                    mpReceivedFormula->realValuedVars(realValuedVars);
                    for( auto varIt = realValuedVars.begin(); varIt != realValuedVars.end(); ++varIt )
                    {
                        if(*varIt != (*variableIt)->var() && (*formulaIt)->constraint().hasVariable(*varIt))
                        {
                            hasAdditionalVariables = true;
                            break;
                        }
                    }
                    if( hasAdditionalVariables)
                    {
                        // cout << "Addidional variables." << endl;
                        for( auto receivedFormulaIt = mpReceivedFormula->begin(); receivedFormulaIt != mpReceivedFormula->end(); ++receivedFormulaIt )
                        {
                            if( (*receivedFormulaIt)->pConstraint()->hasVariable((*variableIt)->var()) && (*receivedFormulaIt)->pConstraint()->isBound() )
                            {
                                reasons.insert(*receivedFormulaIt);
                                // cout << "Also add: " << **receivedFormulaIt << endl;
                            }
                        }
                    }
                    else
                    {
                        // cout << "No additional variables." << endl;
                        auto replacementIt = mDeLinearizations.find( *formulaIt );
                        assert( replacementIt != mDeLinearizations.end() ); // TODO (from Florian): Do we need this?
                        reasons.insert((*replacementIt).second);
                    } // has no additional variables
                } // for all definingOrigins
            }
        }
        return reasons;
    }
    
    PointerSet<Formula> ICPModule::constraintReasonHull( std::set<const Constraint*>& _reasons )
    {
        PointerSet<Formula> reasons;
        for ( auto constraintIt = _reasons.begin(); constraintIt != _reasons.end(); ++constraintIt )
        {
            for ( auto formulaIt = mpReceivedFormula->begin(); formulaIt != mpReceivedFormula->end(); ++formulaIt )
            {
                if ( *constraintIt == (*formulaIt)->pConstraint() )
                {
                    reasons.insert(*formulaIt);
                    break;
                }
            }
        }
        return reasons;
    }
    
    PointerSet<Formula> ICPModule::createConstraintsFromBounds( const EvalDoubleIntervalMap& _map )
    {
        PointerSet<Formula> addedBoundaries;
        Variables originalRealVariables;
        mpReceivedFormula->realValuedVars(originalRealVariables);
        for ( auto variablesIt = originalRealVariables.begin(); variablesIt != originalRealVariables.end(); ++variablesIt )
        {
            const carl::Variable tmpSymbol = *variablesIt;
            if ( _map.find(tmpSymbol) != _map.end() )
            {
                std::map<const carl::Variable, icp::IcpVariable*>::iterator pos = mVariables.find(tmpSymbol);
                if ( pos != mVariables.end() )
                {
                    if ( (*pos).second->isInternalBoundsSet() != icp::Updated::BOTH || (*pos).second->isInternalUpdated() != icp::Updated::NONE )
                    {
                        std::pair<const Constraint*, const Constraint*> boundaries = icp::intervalToConstraint(tmpSymbol, _map.at(tmpSymbol));
                        switch((*pos).second->isInternalBoundsSet())
                        {
                            case icp::Updated::LEFT:
                                if ( boundaries.second != NULL )
                                {
                                    assert( boundaries.second->isConsistent() == 2 );
                                    const Formula* rightBound = newFormula(boundaries.second);
                                    (*pos).second->setInternalRightBound(rightBound);
                                    addedBoundaries.insert(rightBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created upper boundary constraint: " << *rightBound << endl;
                                    #endif
                                }
                                break;                                  
                            case icp::Updated::RIGHT:
                                if ( boundaries.first != NULL)
                                {
                                    assert( boundaries.first->isConsistent() == 2 );
                                    const Formula* leftBound = newFormula(boundaries.first);
                                    (*pos).second->setInternalLeftBound(leftBound);
                                    addedBoundaries.insert(leftBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created lower boundary constraint: " << *leftBound << endl;
                                    #endif
                                }
                                break;
                            case icp::Updated::NONE:
                                if ( boundaries.first != NULL)
                                {
                                    assert( boundaries.first->isConsistent() == 2 );
                                    const Formula* leftBound = newFormula(boundaries.first);
                                    (*pos).second->setInternalLeftBound(leftBound);
                                    addedBoundaries.insert(leftBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created lower boundary constraint: " << *leftBound << endl;
                                    #endif
                                }
                                if ( boundaries.second != NULL )
                                {
                                    assert( boundaries.second->isConsistent() == 2 );
                                    const Formula* rightBound = newFormula(boundaries.second);
                                    (*pos).second->setInternalRightBound(rightBound);
                                    addedBoundaries.insert(rightBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created upper boundary constraint: " << *rightBound << endl;
                                    #endif
                                }
                            default: // Both have been set but some have been updated
                                break;
                        }
                        // check for updates
                        switch((*pos).second->isInternalUpdated())
                        {
                            case icp::Updated::LEFT:
                                if ( boundaries.first != NULL)
                                {
                                    assert( boundaries.first->isConsistent() == 2 );
                                    const Formula* leftBound = newFormula(boundaries.first);
                                    (*pos).second->setInternalLeftBound(leftBound);
                                    addedBoundaries.insert(leftBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created lower boundary constraint: " << *leftBound << endl;
                                    #endif
                                }
                                break;                                  
                            case icp::Updated::RIGHT:
                                if ( boundaries.second != NULL )
                                {
                                    assert( boundaries.second->isConsistent() == 2 );
                                    const Formula* rightBound = newFormula(boundaries.second);
                                    (*pos).second->setInternalRightBound(rightBound);
                                    addedBoundaries.insert(rightBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created upper boundary constraint: " << *rightBound << endl;
                                    #endif
                                }
                                break;
                            case icp::Updated::BOTH:
                                if ( boundaries.first != NULL)
                                {
                                    assert( boundaries.first->isConsistent() == 2 );
                                    const Formula* leftBound = newFormula(boundaries.first);
                                    (*pos).second->setInternalLeftBound(leftBound);
                                    addedBoundaries.insert(leftBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created lower boundary constraint: " << *leftBound << endl;
                                    #endif
                                }
                                if ( boundaries.second != NULL )
                                {
                                    assert( boundaries.second->isConsistent() == 2 );
                                    const Formula* rightBound = newFormula(boundaries.second);
                                    (*pos).second->setInternalRightBound(rightBound);
                                    addedBoundaries.insert(rightBound);
                                    #ifdef ICP_MODULE_DEBUG_1
                                    cout << "Created upper boundary constraint: " << *rightBound << endl;
                                    #endif
                                }
                            default: // none has been updated
                                break;
                        }
                    }
                    else
                    {
                        addedBoundaries.insert((*pos).second->internalLeftBound());
                        addedBoundaries.insert((*pos).second->internalRightBound());
                    }
                }
            }
        }
        return addedBoundaries;
    }
    
    const Formula* ICPModule::transformDeductions( const Formula* _deduction )
    {
        if( _deduction->getType() == CONSTRAINT )
        {
            auto iter = mDeLinearizations.find( _deduction );
            if( iter == mDeLinearizations.end() )
            {
                const Constraint& c = _deduction->constraint();
                return *mCreatedDeductions.insert( newFormula( newConstraint( c.lhs().substitute(mSubstitutions), c.relation()) ) ).first;
            } 
            else
            {
                return iter->second;
            }
        }
        else if( _deduction->getType() == NOT )
        {
            return newNegation( transformDeductions( _deduction->pSubformula() ) );
        }
        else if( _deduction->isBooleanCombination() )
        {
            PointerSet<Formula> subformulas;
            for( const Formula* subformula : _deduction->subformulas() )
            {
                subformulas.insert( transformDeductions( subformula ) );
            }
            const Formula* deduction = newFormula( _deduction->getType(), subformulas );
            mCreatedDeductions.insert( deduction );
            return deduction;
        }
        else
        {
            //should not happen
            assert(false);
            return NULL;
        }
    }
    
    void ICPModule::remapAndSetLraInfeasibleSubsets()
    {
        vec_set_const_pFormula tmpSet = mLRA.infeasibleSubsets();
        for ( auto infSetIt = tmpSet.begin(); infSetIt != tmpSet.end(); ++infSetIt )
        {
            PointerSet<Formula> newSet;
            for ( auto formulaIt = (*infSetIt).begin(); formulaIt != (*infSetIt).end(); ++formulaIt )
            {
                auto delinIt = mDeLinearizations.find(*formulaIt);
                assert( delinIt != mDeLinearizations.end() ); 
                assert( std::find( mpReceivedFormula->begin(), mpReceivedFormula->end(), delinIt->second ) != mpReceivedFormula->end());
                newSet.insert( delinIt->second );
            }
            assert(newSet.size() == (*infSetIt).size());
            mInfeasibleSubsets.push_back(newSet);
        }
    }

    //#ifdef BOXMANAGEMENT
    void ICPModule::setBox( icp::HistoryNode* _selection )
    {
        assert(_selection != NULL);
        #ifdef ICP_MODULE_DEBUG_0
        cout << "Set box -> " << _selection->id() << ", #intervals: " << mIntervals.size() << " -> " << _selection->intervals().size() << endl;
        #endif
        // set intervals - currently we don't change not contained intervals.
        for ( auto constraintIt = _selection->rIntervals().begin(); constraintIt != _selection->rIntervals().end(); ++constraintIt )
        {
            assert(mIntervals.find((*constraintIt).first) != mIntervals.end());
            // only update intervals which changed
            if ( !(mIntervals.at((*constraintIt).first)==(*constraintIt).second) )
            {
                mIntervals[(*constraintIt).first] = (*constraintIt).second;
                std::map<const carl::Variable, icp::IcpVariable*>::const_iterator icpVar = mVariables.find((*constraintIt).first);
                // cout << "Searching for " << (*intervalIt).first.get_name() << endl;
                assert(icpVar != mVariables.end());
                (*icpVar).second->setUpdated();
            }
        }
        // set actual node as selection
        mHistoryActual = _selection;
        mHistoryActual->removeLeftChild();
        mHistoryActual->removeRightChild();
        
        if(mHistoryActual->isLeft())
            mCurrentId = mHistoryActual->id()+1;
        else
            mCurrentId = mHistoryActual->id();
        
        assert(mHistoryActual->isRight() && !mHistoryActual->isLeft());
        if (mHistoryActual->parent() != NULL && mHistoryActual->isRight() )
            mHistoryActual->parent()->removeLeftChild();
    }
    
    icp::HistoryNode* ICPModule::tryToAddConstraint( const ContractionCandidates& _candidates, icp::HistoryNode* _node )
    {
        if(_node != NULL)
        {
            bool contracted = false;
            double relativeContraction;
            EvalDoubleIntervalMap intervals;
            intervals.insert(_node->intervals().begin(), _node->intervals().end());
            assert(intervals.size() != 0);
            for( auto candidateIt = _candidates.begin(); candidateIt !=  _candidates.end(); ++candidateIt )
            {
                relativeContraction = 0;
                tryContraction(*candidateIt, relativeContraction, intervals);
                contracted = relativeContraction > 0;
                if(contracted)
                    break;
            }
            if (contracted)
                return _node;
            else
            {
                // left-most outer-most
                icp::HistoryNode* success = tryToAddConstraint(_candidates, _node->left());
                if (success == NULL)
                    success = tryToAddConstraint(_candidates, _node->right());
                return success;
            }
        }
        return NULL;
    }
    
    PointerSet<Formula> ICPModule::collectReasons( icp::HistoryNode* _node )
    {
        icp::set_icpVariable variables = _node->rStateInfeasibleVariables();
        for( auto varIt = variables.begin(); varIt != variables.end(); ++varIt )
        {
            // cout << "Collect Hull for " << (*varIt)->var().get_name() << endl;
            _node->variableHull((*varIt)->var(), variables);
        }
        PointerSet<Formula> reasons = variableReasonHull(variables);
        PointerSet<Formula> constraintReasons = constraintReasonHull(_node->rStateInfeasibleConstraints());
        reasons.insert(constraintReasons.begin(), constraintReasons.end());
        return reasons;
    }
    //#endif
    
    #ifdef ICP_BOXLOG
    void ICPModule::writeBox()
    {
        GiNaC::symtab originalRealVariables = mpReceivedFormula->realValuedVars();
        
        for ( auto varIt = originalRealVariables.begin(); varIt != originalRealVariables.end(); ++varIt )
        {
            icpLog << "; " << (*varIt).first;
            if ( mIntervals.find(ex_to<symbol>((*varIt).second)) != mIntervals.end() )
            {
                icpLog << "[";
                if ( mIntervals[ex_to<symbol>((*varIt).second)].lowerBoundType() == carl::BoundType::INFTY )
                {
                    icpLog << "INF";
                }
                else
                {
                    icpLog << mIntervals[ex_to<symbol>((*varIt).second)].lower();
                }
                icpLog << ",";
                if ( mIntervals[ex_to<symbol>((*varIt).second)].upperBoundType() == carl::BoundType::INFTY )
                {
                    icpLog << "INF";
                }
                else
                {
                    icpLog << mIntervals[ex_to<symbol>((*varIt).second)].upper();
                }
                icpLog << "]";
            }
        }
        icpLog << "\n";
    }
    #endif
    
    void ICPModule::debugPrint()
    {
        cout << "********************* linear Constraints **********************" << endl;
        for( auto linearIt = mLinearConstraints.begin(); linearIt != mLinearConstraints.end(); ++linearIt){
            for ( auto candidateIt = (*linearIt).second.begin(); candidateIt != (*linearIt).second.end(); ++candidateIt )
            {
                const Constraint* constraint = (*candidateIt)->constraint();
                cout << (*candidateIt)->id() << ": " << *constraint << endl;
            }
        }
        cout << "****************** active linear constraints ******************" << endl;
        for(auto activeLinearIt = mActiveLinearConstraints.begin(); activeLinearIt != mActiveLinearConstraints.end(); ++activeLinearIt){
            cout << "Count: " << (*activeLinearIt)->activity() << " , ";
            (*activeLinearIt)->print();
        }
        cout << "******************* active linear variables *******************" << endl;
        for (auto variableIt = mVariables.begin(); variableIt != mVariables.end(); ++variableIt )
        {
            if ( (*variableIt).second->checkLinear() )
                cout << (*variableIt).first << ", ";
        }
        cout << endl;
        cout << "******************** nonlinear constraints ********************" << endl;
        std::map<const Constraint*, ContractionCandidates>::iterator nonlinearIt;
        ContractionCandidates::iterator replacementsIt;

        for(nonlinearIt = mNonlinearConstraints.begin(); nonlinearIt != mNonlinearConstraints.end(); ++nonlinearIt){
            cout << *(*nonlinearIt).first << endl;
            cout << "\t replacements: " << endl;
            for(replacementsIt = nonlinearIt->second.begin(); replacementsIt != nonlinearIt->second.end(); ++replacementsIt)
            {
                cout << "   ";
                (*replacementsIt)->print();
            }
        }
        cout << "**************** active nonlinear constraints *****************" << endl;
        for( auto activeNonlinearIt = mActiveNonlinearConstraints.begin(); activeNonlinearIt != mActiveNonlinearConstraints.end(); ++activeNonlinearIt )
        {
            cout << "Count: " << (*activeNonlinearIt)->activity() << " , ";
            (*activeNonlinearIt)->print();
        }
        cout << "***************** active nonlinear variables ******************" << endl;
        for (auto variableIt = mVariables.begin(); variableIt != mVariables.end(); ++variableIt )
        {
            if ( (*variableIt).second->checkLinear() == false )
                cout << (*variableIt).first << ", ";
        }
        cout << endl;
        cout << "************************** Intervals **************************" << endl;
        for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
        {
            cout << (*constraintIt).first << "  \t -> \t" << (*constraintIt).second << endl;
        }
        cout << endl;
        cout << "************************* Linearizations ************************" << endl;
        for ( auto replacementIt = mLinearizations.begin(); replacementIt != mLinearizations.end(); ++replacementIt )
        {
            cout << *(*replacementIt).first << "  \t -> \t" << *(*replacementIt).second << endl;
        }
        cout <<endl;
        cout << "************************* Delinearizations ************************" << endl;
        for ( auto replacementIt = mDeLinearizations.begin(); replacementIt != mDeLinearizations.end(); ++replacementIt )
        {
            cout << *(*replacementIt).first << "  \t -> \t" << *(*replacementIt).second << endl;
        }
        cout <<endl;
        cout << "************************* ICP Variables ***********************" << endl;
        for ( auto variablesIt = mVariables.begin(); variablesIt != mVariables.end(); ++variablesIt )
            (*variablesIt).second->print();
        
        cout << endl;
        cout << "*********************** ValidationFormula *********************" << endl;
        cout << mValidationFormula->toString() << endl;
        cout << "***************************************************************" << endl;
        
        cout << "************************* Substitution ************************" << endl;
        for( auto subsIt = mSubstitutions.begin(); subsIt != mSubstitutions.end(); ++subsIt )
            cout << (*subsIt).first << " -> " << (*subsIt).second << endl;
        
        cout << "***************************************************************" << endl;
    }
    
    void ICPModule::printAffectedCandidates()
    {
        for ( auto varIt = mVariables.begin(); varIt != mVariables.end(); ++varIt )
        {
            for ( auto candidateIt = (*varIt).second->candidates().begin(); candidateIt != (*varIt).second->candidates().end(); ++candidateIt)
            {
                cout << (*varIt).first << "\t -> ";
                (*candidateIt)->print();
            }
        }
    }

    void ICPModule::printIcpVariables()
    {
        for ( auto varIt = mVariables.begin(); varIt != mVariables.end(); ++varIt )
            (*varIt).second->print();
    }

    void ICPModule::printIcpRelevantCandidates()
    {
        cout << "Size icpRelevantCandidates: " << mIcpRelevantCandidates.size() << endl;
        for ( auto candidateIt = mIcpRelevantCandidates.begin(); candidateIt != mIcpRelevantCandidates.end(); ++candidateIt )
        {
            cout << (*candidateIt).first << " \t " << (*candidateIt).second <<"\t Candidate: ";
            mCandidateManager->getInstance()->getCandidate((*candidateIt).second)->print();
        }
    }

    void ICPModule::printIntervals( bool _original )
    {
        for ( auto constraintIt = mIntervals.begin(); constraintIt != mIntervals.end(); ++constraintIt )
        {
            auto varIt = mVariables.find((*constraintIt).first);
            //assert( varIt != mVariables.end() );//TODO (from FLorian): can we assume this?
            if( !_original || (varIt != mVariables.end() && varIt->second->isOriginal()))
            {
                cout << (*constraintIt).first << " \t -> " << (*constraintIt).second << endl;
            }
        }
    }
} // namespace smtrat
