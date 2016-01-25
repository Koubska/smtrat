/**
 * @file ESModule.tpp
 * @author Florian Corzilius <corzilius@cs.rwth-aachen.de>
 *
 * @version 2015-09-09
 * Created on 2015-09-09.
 */

//#define DEBUG_ELIMINATE_SUBSTITUTIONS

#include "ESModule.h"

namespace smtrat
{
    template<class Settings>
    ESModule<Settings>::ESModule( const ModuleInput* _formula, RuntimeSettings*, Conditionals& _conditionals, Manager* _manager ):
        PModule( _formula, _conditionals, _manager ),
        mBoolSubs(),
        mArithSubs()
    {}

    template<class Settings>
    ESModule<Settings>::~ESModule()
    {}

    template<class Settings>
    void ESModule<Settings>::updateModel() const
    {
        clearModel();
        if( solverState() == SAT || (solverState() != UNSAT && appliedPreprocessing()) )
        {
            getBackendsModel();
            for( const auto& iter : mBoolSubs )
            {
                if( iter.first.getType() == carl::FormulaType::BOOL )
                {
                    assert( mModel.find( iter.first.boolean() ) == mModel.end() );
                    mModel.emplace( iter.first.boolean(), iter.second );
                }
            }
            for( const auto& iter : mArithSubs )
            {
                assert( mModel.find( iter.first ) == mModel.end() );
                mModel.emplace( iter.first, vs::SqrtEx( iter.second ) );
            }
        }
    }

    template<class Settings>
    Answer ESModule<Settings>::checkCore()
    {
        mBoolSubs.clear();
        mArithSubs.clear();
        FormulaT formula = elimSubstitutions( (FormulaT) rReceivedFormula(), true, true );
        Answer ans = SAT;
        if( formula.isFalse() )
            ans = UNSAT;
        else
        {
            addSubformulaToPassedFormula( formula );
            ans = runBackends();
        }
        if( ans == UNSAT )
            generateTrivialInfeasibleSubset(); // TODO: compute a better infeasible subset
        return ans;
    }
    
    template<typename Settings>
    FormulaT ESModule<Settings>::elimSubstitutions( const FormulaT& _formula, bool _elimSubstitutions, bool _outermost ) 
    {
        
        auto iter = mBoolSubs.find( _formula );
        if( iter != mBoolSubs.end() )
        {
			SMTRAT_LOG_DEBUG("smtrat.es", _formula << " ----> " << (iter->second ? FormulaT( carl::FormulaType::TRUE ) : FormulaT( carl::FormulaType::FALSE )));
            return iter->second ? FormulaT( carl::FormulaType::TRUE ) : FormulaT( carl::FormulaType::FALSE );
        }
        FormulaT result = _formula;
        switch( _formula.getType() )
        {
            case carl::FormulaType::AND:
            {
                std::vector<std::map<carl::Variable,Poly>::const_iterator> addedArithSubs;
                std::unordered_map<FormulaT,std::unordered_map<FormulaT, bool>::const_iterator> foundBooleanSubstitutions;
                bool foundNewSubstitution = true;
                FormulaSetT foundSubstitutions;
                FormulasT currentSubformulas = result.subformulas();
                while( foundNewSubstitution )
                {
                    FormulasT sfs;
                    foundNewSubstitution = false;
                    // Process all equations first.
                    for( const auto& sf : currentSubformulas )
                    {
                        if( sf.getType() == carl::FormulaType::CONSTRAINT && sf.constraint().relation() == carl::Relation::EQ && sf.constraint().lhs().isLinear() )
                        {
                            FormulaT tmp = elimSubstitutions( sf );
                            if( tmp.getType() == carl::FormulaType::FALSE )
                            {
                                result = tmp;
                                goto Return;
                            }
                            else if( tmp.getType() != carl::FormulaType::TRUE )
                            {
                                carl::Variable subVar;
                                Poly subPoly;
                                if( tmp.constraint().getSubstitution( subVar, subPoly ) )
                                {
                                    SMTRAT_LOG_DEBUG("smtrat.es", "found substitution [" << subVar << " -> " << subPoly << "]");
                                    assert( mArithSubs.find( subVar ) == mArithSubs.end() );
                                    addedArithSubs.push_back( mArithSubs.emplace( subVar, subPoly ).first );
                                    foundSubstitutions.insert( tmp );
                                    foundNewSubstitution = true;
                                }
                                else
                                {
                                    sfs.push_back( tmp );
                                }
                            }
                        }
                    }
                    // Now the other sub-formulas.
                    for( const auto& sf : currentSubformulas )
                    {
                        if( sf.getType() != carl::FormulaType::CONSTRAINT || sf.constraint().relation() != carl::Relation::EQ || !sf.constraint().lhs().isLinear() )
                        {
                            auto iterC = foundBooleanSubstitutions.find( sf );
                            if( iterC != foundBooleanSubstitutions.end() )
                            {
                                mBoolSubs.erase( iterC->second );
                                foundBooleanSubstitutions.erase( iterC );
                            }
                            FormulaT sfSimplified = elimSubstitutions( sf );
                            if( sfSimplified.isFalse() )
                            {
                                result = sfSimplified;
                                goto Return;
                            }
                            else if( !sfSimplified.isTrue() )
                            {
                                if( sf != sfSimplified )
                                {
                                    foundNewSubstitution = true;
                                    if( sfSimplified.getType() == carl::FormulaType::AND )
                                    {
                                        sfs.insert(sfs.end(), sfSimplified.subformulas().begin(), sfSimplified.subformulas().end() );
                                    }
                                    else
                                        sfs.push_back( sfSimplified );
                                }
                                else
                                {
                                    if( !_outermost || !sfSimplified.isLiteral() || !sfSimplified.isOnlyPropositional() )
                                        sfs.push_back( sfSimplified );
                                    if( sfSimplified.getType() == carl::FormulaType::NOT )
                                    {
                                        SMTRAT_LOG_DEBUG("smtrat.es", "found boolean substitution [" << sfSimplified.subformula() << " -> false]");
                                        assert( mBoolSubs.find( sfSimplified.subformula() ) == mBoolSubs.end() );
                                        assert( foundBooleanSubstitutions.find( sfSimplified ) == foundBooleanSubstitutions.end() );
                                        foundBooleanSubstitutions.emplace( sfSimplified, mBoolSubs.insert( std::make_pair( sfSimplified.subformula(), false ) ).first );
                                    }
                                    else
                                    {
                                        SMTRAT_LOG_DEBUG("smtrat.es", "found boolean substitution [" << sfSimplified << " -> true]");
                                        assert( mBoolSubs.find( sfSimplified ) == mBoolSubs.end() );
                                        assert( foundBooleanSubstitutions.find( sfSimplified ) == foundBooleanSubstitutions.end() );
                                        foundBooleanSubstitutions.emplace( sfSimplified, mBoolSubs.insert( std::make_pair( sfSimplified, true ) ).first );
                                    }
                                }
                            }
                        }
                    }
                    currentSubformulas = std::move(sfs);
                }
                if( currentSubformulas.empty() )
                {
                    if( foundSubstitutions.empty() )
                        result = FormulaT( carl::FormulaType::TRUE );
                    else if( !_elimSubstitutions )
                        result = FormulaT( carl::FormulaType::AND, std::move(foundSubstitutions) );
                }
                else
                {
                    if( !_elimSubstitutions )
                        currentSubformulas.insert(currentSubformulas.end(), foundSubstitutions.begin(), foundSubstitutions.end() );
                    result = FormulaT( carl::FormulaType::AND, std::move(currentSubformulas) );
                }
            Return:
                if( !_outermost )
                {
                    while( !addedArithSubs.empty() )
                    {
                        assert( std::count( addedArithSubs.begin(), addedArithSubs.end(), addedArithSubs.back() ) == 1 );
                        mArithSubs.erase( addedArithSubs.back() );
                        addedArithSubs.pop_back();
                    }
                    while( !foundBooleanSubstitutions.empty() )
                    {
                        mBoolSubs.erase( foundBooleanSubstitutions.begin()->second );
                        foundBooleanSubstitutions.erase( foundBooleanSubstitutions.begin() );
                    }
                }
                break;
            }
            case carl::FormulaType::ITE:
            {
                FormulaT cond = elimSubstitutions( _formula.condition() );
                if( cond.getType() == carl::FormulaType::CONSTRAINT )
                {
                    carl::Variable subVar;
                    Poly subPoly;
                    if( cond.constraint().getSubstitution( subVar, subPoly, false ) )
                    {
                        #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                        std::cout << __LINE__ << "   found substitution [" << subVar << " -> " << subPoly << "]" << std::endl;
                        #endif
                        auto addedBoolSub = cond.getType() == carl::FormulaType::NOT ? mBoolSubs.emplace( cond.subformula(), false ) : mBoolSubs.emplace( cond, true );
                        #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                        std::cout <<  __LINE__ << "   found boolean substitution [" << addedBoolSub.first->first << " -> " << (addedBoolSub.first->second ? "true" : "false") << "]" << std::endl;
                        #endif
                        assert( addedBoolSub.second );
                        auto iterB = mArithSubs.emplace( subVar, subPoly ).first;
                        FormulaT firstCaseTmp = elimSubstitutions( _formula.firstCase() );
                        mArithSubs.erase( iterB );
                        mBoolSubs.erase( addedBoolSub.first );
                        addedBoolSub = cond.getType() == carl::FormulaType::NOT ? mBoolSubs.emplace( cond.subformula(), true ) : mBoolSubs.emplace( cond, false );
                        #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                        std::cout <<  __LINE__ << "   found boolean substitution [" << addedBoolSub.first->first << " -> " << (addedBoolSub.first->second ? "true" : "false") << "]" << std::endl;
                        #endif
                        assert( addedBoolSub.second );
                        FormulaT secondCaseTmp = elimSubstitutions( _formula.secondCase() );
                        mBoolSubs.erase( addedBoolSub.first );
                        result = FormulaT( carl::FormulaType::ITE, {cond, firstCaseTmp, secondCaseTmp} );
                        break;
                    }
                    else if( cond.constraint().getSubstitution( subVar, subPoly, true ) )
                    {
                        #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                        std::cout << __LINE__ << "   found substitution [" << subVar << " -> " << subPoly << "]" << std::endl;
                        #endif
                        auto addedBoolSub = cond.getType() == carl::FormulaType::NOT ? mBoolSubs.emplace( cond.subformula(), false ) : mBoolSubs.emplace( cond, true );
                        #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                        std::cout <<  __LINE__ << "   found boolean substitution [" << addedBoolSub.first->first << " -> " << (addedBoolSub.first->second ? "true" : "false") << "]" << std::endl;
                        #endif
                        assert( addedBoolSub.second );
                        FormulaT firstCaseTmp = elimSubstitutions( _formula.firstCase() );
                        mBoolSubs.erase( addedBoolSub.first );
                        addedBoolSub = cond.getType() == carl::FormulaType::NOT ? mBoolSubs.emplace( cond.subformula(), true ) : mBoolSubs.emplace( cond, false );
                        #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                        std::cout <<  __LINE__ << "   found boolean substitution [" << addedBoolSub.first->first << " -> " << (addedBoolSub.first->second ? "true" : "false") << "]" << std::endl;
                        #endif
                        assert( addedBoolSub.second );
                        auto iterB = mArithSubs.emplace( subVar, subPoly ).first;
                        FormulaT secondCaseTmp = elimSubstitutions( _formula.secondCase() );
                        mArithSubs.erase( iterB );
                        mBoolSubs.erase( addedBoolSub.first );
                        result = FormulaT( carl::FormulaType::ITE, {cond, firstCaseTmp, secondCaseTmp} );
                        break;
                    }
                }
                if( cond.isTrue() )
                {
                    result = elimSubstitutions( _formula.firstCase() );
                }
                else if( cond.isFalse() )
                {
                    result = elimSubstitutions( _formula.secondCase() );
                }
                else
                {
                    auto addedBoolSub = cond.getType() == carl::FormulaType::NOT ? mBoolSubs.emplace( cond.subformula(), false ) : mBoolSubs.emplace( cond, true );
                    #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                    std::cout <<  __LINE__ << "   found boolean substitution [" << addedBoolSub.first->first << " -> " << (addedBoolSub.first->second ? "true" : "false") << "]" << std::endl;
                    #endif
                    assert( addedBoolSub.second );
                    FormulaT firstCaseTmp = elimSubstitutions( _formula.firstCase() );
                    mBoolSubs.erase( addedBoolSub.first );
                    addedBoolSub = cond.getType() == carl::FormulaType::NOT ? mBoolSubs.emplace( cond.subformula(), true ) : mBoolSubs.emplace( cond, false );
                    #ifdef DEBUG_ELIMINATE_SUBSTITUTIONS
                    std::cout <<  __LINE__ << "   found boolean substitution [" << addedBoolSub.first->first << " -> " << (addedBoolSub.first->second ? "true" : "false") << "]" << std::endl;
                    #endif
                    assert( addedBoolSub.second );
                    FormulaT secondCaseTmp = elimSubstitutions( _formula.secondCase() );
                    mBoolSubs.erase( addedBoolSub.first );
                    result = FormulaT( carl::FormulaType::ITE, {cond, firstCaseTmp, secondCaseTmp} );
                }
                break;
            }
            case carl::FormulaType::OR:
            case carl::FormulaType::IFF:
            case carl::FormulaType::XOR: {
                FormulasT newSubformulas;
                bool changed = false;
                for (const auto& cur: _formula.subformulas()) {
                    FormulaT newCur = elimSubstitutions(cur);
                    if (newCur != cur) changed = true;
                    newSubformulas.push_back(newCur);
                }
                if (changed)
                    result = FormulaT(_formula.getType(), std::move(newSubformulas));
                break;
            }
            case carl::FormulaType::NOT: {
                FormulaT cur = elimSubstitutions(_formula.subformula());
                if (cur != _formula.subformula())
                    result = FormulaT(carl::FormulaType::NOT, cur);
                break;
            }
            case carl::FormulaType::IMPLIES: {
                FormulaT prem = elimSubstitutions(_formula.premise());
                FormulaT conc = elimSubstitutions(_formula.conclusion());
                if ((prem != _formula.premise()) || (conc != _formula.conclusion()))
                    result = FormulaT(carl::FormulaType::IMPLIES, {prem, conc});
                break;
            }
            case carl::FormulaType::CONSTRAINT: {
                FormulaT tmp = result;
                while( result != (tmp = tmp.substitute( mArithSubs )) )
                    result = tmp;
                break;
            }
            case carl::FormulaType::BOOL:
            case carl::FormulaType::BITVECTOR:
            case carl::FormulaType::UEQ: 
            case carl::FormulaType::TRUE:
            case carl::FormulaType::FALSE:
				SMTRAT_LOG_DEBUG("smtrat.es", _formula << " ----> " << result);
                return result;
            case carl::FormulaType::EXISTS:
            case carl::FormulaType::FORALL: {
                FormulaT sub = elimSubstitutions(_formula.quantifiedFormula());
                if (sub != _formula.quantifiedFormula())
                    result = FormulaT(_formula.getType(), _formula.quantifiedVariables(), sub);
            }
        }
        iter = mBoolSubs.find( result );
        if( iter != mBoolSubs.end() )
        {
			SMTRAT_LOG_DEBUG("smtrat.es", _formula << " ----> " << (iter->second ? FormulaT( carl::FormulaType::TRUE ) : FormulaT( carl::FormulaType::FALSE )));
            return iter->second ? FormulaT( carl::FormulaType::TRUE ) : FormulaT( carl::FormulaType::FALSE );
        }
		SMTRAT_LOG_DEBUG("smtrat.es", _formula << " ----> " << result);
        return result;
    }
}

#include "Instantiation.h"
