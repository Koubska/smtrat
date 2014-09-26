/*
 * SMT-RAT - Satisfiability-Modulo-Theories Real Algebra Toolbox
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
 * @file   PreprocessingModule.cpp
 * @author: Sebastian Junges
 *
 *
 */

#include "PreprocessingModule.h"
#include "../../../cli/ExitCodes.h"
#include <limits.h>
#include <bits/stl_map.h>

//#define REMOVE_LESS_EQUAL_IN_CNF_TRANSFORMATION (Not working)
//#define ADDLINEARDEDUCTIONS
//#define PREPROCESSING_DEVELOP_MODE

namespace smtrat {
PreprocessingModule::PreprocessingModule( ModuleType _type, const Formula* const _formula, RuntimeSettings* settings, Conditionals& _conditionals, Manager* const _manager ):
        Module( _type, _formula, _conditionals, _manager )
    {

    }

    /**
     * Destructor:
     */
    PreprocessingModule::~PreprocessingModule(){}

    /**
     * Methods:
     */

    /**
     * Adds a constraint to this module.
     *
     * @param _constraint The constraint to add to the already added constraints.
     *
     * @return true
     */
    bool PreprocessingModule::assertSubformula( Formula::const_iterator _subformula )
    {
        Module::assertSubformula( _subformula );
        return true;
    }

    /**
     * Checks the so far received constraints for consistency.
     */
    Answer PreprocessingModule::isConsistent()
    {
        //mpReceivedFormula->print();

        Formula::const_iterator receivedSubformula = firstUncheckedReceivedSubformula();
        while( receivedSubformula != mpReceivedFormula->end() )
        {
            Formula* afterProductSplitting = new Formula( **receivedSubformula );
            // Inequations are transformed.
            rewritePotentialInequalities( afterProductSplitting );
            #ifdef ADDLINEARDEDUCTIONS
            if( afterProductSplitting->getType() == AND )
            {
                addLinearDeductions( afterProductSplitting );
            }
            #endif
            // Add the currently considered formula of the received constraint as clauses
            // to the passed formula.
            Formula::toCNF( *afterProductSplitting, false );
            // Estimate the difficulty bottum up for the formula.
            setDifficulty(afterProductSplitting,false);

            if( afterProductSplitting->getType() == TTRUE )
            {
                // No need to add it.
            }
            else if( afterProductSplitting->getType() == FFALSE )
            {
                // Infeasible subset missing?
                return foundAnswer( False );
            }
            else
            {
                if( afterProductSplitting->getType() == AND )
                {
                    while( !afterProductSplitting->empty() )
                    {
                        addSubformulaToPassedFormula( afterProductSplitting->pruneBack(), *receivedSubformula );
                    }
                    delete afterProductSplitting;
                }
                else
                {
                    addSubformulaToPassedFormula( afterProductSplitting, *receivedSubformula );
                }
            }
            ++receivedSubformula;
        }
        assignActivitiesToPassedFormula();
        //mpPassedFormula->print();

        // Call backends.
        Answer ans = runBackends();
        if( ans == False )
        {
            getInfeasibleSubsets();
        }
        return foundAnswer( ans );
    }

    /**
     * Removes a everything related to a sub formula of the received formula.
     *
     * @param _subformula The sub formula of the received formula to remove.
     */
    void PreprocessingModule::removeSubformula( Formula::const_iterator _subformula )
    {
        Module::removeSubformula( _subformula );
    }

    /**
     * Res
     * @param formula
     * @param invert
     */
    void PreprocessingModule::rewritePotentialInequalities( Formula* formula, bool invert )
    {
        if( formula->getType() == NOT )
        {
            assert( formula->subformulas().size() == 1 );
            Formula* subformula = formula->subformulas().front();
            if(subformula->isBooleanCombination())
            {
                rewritePotentialInequalities(subformula, !invert);
            }
            else if(subformula->getType() == CONSTRAINT)
            {
                const Constraint* constraint = subformula->pConstraint();
                // Since we are considering a not, invert is in fact "inverted" ;-)
                if(!invert)
                {
                    formula->pop_back();
                    switch( constraint->relation() )
                    {
                        case CR_EQ:
                        {
                            formula->copyAndDelete( new Formula( OR ));
                            formula->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_LESS, constraint->variables() )));
                            formula->addSubformula( new Formula( newConstraint( -constraint->lhs(), CR_LESS, constraint->variables() )));
                            return;
                        }
                        case CR_LEQ:
                        {
                            formula->copyAndDelete( new Formula( newConstraint( -constraint->lhs(), CR_LESS, constraint->variables() )));
                            return;
                        }
                        case CR_LESS:
                        {
                            #ifdef REMOVE_LESS_EQUAL_IN_CNF_TRANSFORMATION
                            formula->copyAndDelete( new Formula( OR ));
                            formula->addSubformula( new Formula( newConstraint( -constraint->lhs(), CR_LESS, constraint->variables() )));
                            formula->addSubformula( new Formula( newConstraint( -constraint->lhs(), CR_EQ, constraint->variables() )));
                            #else
                            formula->copyAndDelete( new Formula( newConstraint( -constraint->lhs(), CR_LEQ, constraint->variables() )));
                            #endif
                            return;
                        }
                        case CR_NEQ:
                        {
                            formula->copyAndDelete( new Formula( newConstraint( constraint->lhs(), CR_EQ, constraint->variables() )));
                            return;
                        }
                        default:
                        {
                            std::cerr << "Unexpected relation symbol!" << std::endl;
                            exit(SMTRAT_EXIT_GENERALERROR);
                        }
                    }
                }
                #ifdef REMOVE_LESS_EQUAL_IN_CNF_TRANSFORMATION
                else
                {
                    switch( constraint->relation() )
                    {
                        case CR_EQ:
                        {
                            return;
                        }
                        case CR_LEQ:
                        {
                            subformula->copyAndDelete( new Formula( OR ));
                            subformula->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_LESS, constraint->variables() )));
                            subformula->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_EQ, constraint->variables() )));
                            return;
                        }
                        case CR_LESS:
                        {
                            return;

                        }
                        case CR_NEQ:
                        {
                            subformula->copyAndDelete( new Formula( OR ));
                            subformula->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_LESS, constraint->variables() )));
                            subformula->addSubformula( new Formula( newConstraint( -constraint->lhs(), CR_LESS, constraint->variables() )));
                            return;
                        }
                        default:
                        {
                            std::cerr << "Unexpected relation symbol!" << std::endl;
                            exit(SMTRAT_EXIT_GENERALERROR);
                        }
                    }
                }
                #endif
            }
        }
        else if( formula->getType() == OR || formula->getType() == AND || formula->getType() == XOR || formula->getType() == IFF  )
        {
            for( std::list<Formula*>::const_iterator it = formula->subformulas().begin(); it != formula->subformulas().end(); ++it )
            {
                rewritePotentialInequalities(*it, invert);
            }
        }
        #ifdef REMOVE_LESS_EQUAL_IN_CNF_TRANSFORMATION
        else if( formula->getType() == CONSTRAINT )
        {
            formula->print();
            const Constraint* constraint = formula->pConstraint();

            switch( constraint->relation() )
            {
                case CR_EQ:
                {
                    return;
                }
                case CR_LEQ:
                {
                    Formula* phi = new Formula(OR );
                    phi->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_EQ, constraint->variables() )));
                    phi->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_LESS, constraint->variables() )));
                    formula->pFather()->addSubformula(phi);
                    formula->father().print();
                    formula->pFather()->erase(formula);
                }
                case CR_LESS:
                {
                    return;

                }
                case CR_NEQ:
                {
                    formula->copyAndDelete( new Formula( OR ));
                    formula->addSubformula( new Formula( newConstraint( constraint->lhs(), CR_LESS, constraint->variables() )));
                    formula->addSubformula( new Formula( newConstraint( -constraint->lhs(), CR_LESS, constraint->variables() )));
                    return;
                }
                default:
                {
                    std::cerr << "Unexpected relation symbol!" << std::endl;
                    exit(SMTRAT_EXIT_GENERALERROR);
                }
            }
        }
        #endif

        return;

    }

    void PreprocessingModule::setDifficulty(Formula* formula, bool invert)
    {
        if( formula->getType() == NOT )
        {
            setDifficulty(formula->subformulas().front(), !invert);
            formula->setDifficulty(formula->subformulas().front()->difficulty());
        }

        if( (formula->getType() == AND && !invert) || (formula->getType() == OR && invert) )
        {
            double maxdifficulty = 0;
            double sumdifficulty = 0;
            double subformulaDifficulty = 0;
            for( std::list<Formula*>::const_iterator it = formula->subformulas().begin(); it != formula->subformulas().end(); ++it )
            {
                setDifficulty(*it, invert);
                subformulaDifficulty = (*it)->difficulty();
                if( subformulaDifficulty > maxdifficulty )
                {
                    maxdifficulty = subformulaDifficulty;
                }
                sumdifficulty += subformulaDifficulty;
            }
            formula->setDifficulty(sumdifficulty + maxdifficulty);
        }
        else if( (formula->getType() == OR && !invert) || (formula->getType() == AND && invert) )
        {
            double difficulty = 2000000; // TODO enter bound here.
            for( std::list<Formula*>::const_iterator it = formula->subformulas().begin(); it != formula->subformulas().end(); ++it )
            {
                setDifficulty(*it, invert);
                if( (*it)->difficulty() < difficulty )
                {
                    difficulty = (*it)->difficulty();
                }
            }
            formula->setDifficulty(difficulty);
        }
        else if( formula->getType() == IMPLIES  )
        {
            assert(formula->subformulas().size() == 2);
            double difficulty = formula->subformulas().front()->difficulty() + formula->subformulas().back()->difficulty();
            formula->setDifficulty(difficulty/2);
        }
        else if( formula->getType() == IFF )
        {
            assert(formula->subformulas().size() == 2);
            double difficulty = formula->subformulas().front()->difficulty() + formula->subformulas().back()->difficulty();
            formula->setDifficulty(difficulty/2);
        }
        else if( formula->getType() == XOR )
        {
            assert(formula->subformulas().size() == 2);
            double difficulty = std::max(formula->subformulas().front()->difficulty(), formula->subformulas().back()->difficulty());
            formula->setDifficulty(difficulty);
        }
        else if( formula->getType() == CONSTRAINT )
        {
            const Constraint* constraint = formula->pConstraint();
            double difficulty;
            if( constraint->isLinear() )
            {
                difficulty = 20;
            }
            else
            {
                difficulty = 300;
            }
            // Equalities allow for a small solution space, so we find them easier.

            difficulty += (constraint->numMonomials()-1) * 8;
            if( constraint->relation() == CR_EQ )
            {
                difficulty *= 0.7;
            }
            if( constraint->relation() == CR_LEQ  || constraint->relation() == CR_GEQ)
            {
                difficulty *= 1.1;
            }
            formula->setDifficulty(difficulty);
        }
    }

    void PreprocessingModule::assignActivitiesToPassedFormula()
    {
        double globalMaxDifficulty = 0;
        for( std::list<Formula*>::const_iterator it = mpPassedFormula->subformulas().begin(); it != mpPassedFormula->subformulas().end(); ++it )
        {
            if((*it)->getType() != OR) continue;
            else
            {
                for( std::list<Formula*>::const_iterator jt = (*it)->subformulas().begin(); jt != (*it)->subformulas().end(); ++jt )
                {
                    if( (*jt)->difficulty() > globalMaxDifficulty )
                    {
                        globalMaxDifficulty = (*jt)->difficulty();
                    }
                }
            }
        }

        for( std::list<Formula*>::const_iterator it = mpPassedFormula->subformulas().begin(); it != mpPassedFormula->subformulas().end(); ++it )
        {
            if((*it)->getType() != OR) continue;
            else
            {
                for( std::list<Formula*>::const_iterator jt = (*it)->subformulas().begin(); jt != (*it)->subformulas().end(); ++jt )
                {
                    // Special treatment for identities.
                    if( (*jt)->getType() == CONSTRAINT )
                    {
                        const Constraint* constraint = (*jt)->pConstraint();

                        if(constraint->relation() == CR_EQ && constraint->isLinear() && constraint->numMonomials() <= 20)
                        {
                            (*jt)->setActivity(-100);
                            continue;
                        }

                    }
                    // Otherwise we just set the activity according to the difficulty.
                    (*jt)->setActivity( 100 * ((*jt)->difficulty()/globalMaxDifficulty) );
                }
            }
        }
        // set certain activities negative, such that the sat solver knows that they should preferably be send to the tsolver.
//        for( std::list<Formula*>::const_iterator it = mpPassedFormula->subformulas().begin(); it != mpPassedFormula->subformulas().end(); ++it )
//        {
//            if((*it)->getType() != OR) continue;
//            else
//            {
//                for( std::list<Formula*>::const_iterator jt = (*it)->subformulas().begin(); jt != (*it)->subformulas().end(); ++jt )
//                {
//                    if( (*jt)->difficulty() < globalMaxDifficulty / 2 ) {
//                    //    (*jt)->setActivity(-(*jt)->activity());
//                    }
//                }
//            }
//        }

    }

    /**
     * Notice: This method has not been finished yet.
     *
     * Search in the current AND formula for real constraints which have to hold.
     * If the are nonlinear, we try to find linear equations which can be deduced from this and add them to the formula.
     * @param formula
     */
    void PreprocessingModule::addLinearDeductions(Formula* formula)
    {
        assert(formula->getType() == AND);
        for( std::list<Formula*>::const_iterator it = formula->subformulas().begin(); it != formula->subformulas().end(); ++it )
        {
            if((*it)->getType() == CONSTRAINT)
            {
                const Constraint* constraint = (*it)->pConstraint();
                // If we already have a linear equation, we are not going to extract other linear equations from it
                if( constraint->isLinear() )
                {
                    continue;
                }
                unsigned degree = constraint->maxMonomeDegree();
                // This are constraints of the form a1t_1 ~ 0 with a1 a numerical value and t_1 a monomial of degree > 1.
                if( constraint->numMonomials() == 1 )
                {
                    #ifdef PREPROCESSING_DEVELOP_MODE
                    assert(false)
                    #endif
                    // TODO implement this.
                }
                // We search for constraints of the form a1t_1 + a2 ~ 0
                // with a being numerical values and t_1 a monomial of degree > 1.
                else if( constraint->numMonomials() == 2 )
                {
                    GiNaC::ex expression = constraint->lhs();

                    GiNaC::numeric constPart = constraint->constantPart() ;
                    assert( GiNaC::is_exactly_a<GiNaC::add>( expression ) );

                    // We look for a_1
                    GiNaC::const_iterator term = expression.begin();
                    // If it is the constant part, skip.
                    if(GiNaC::is_exactly_a<GiNaC::numeric>(*term)) ++term;
                    // If it is a power, it is a power of a variable only, so the coefficient would be 1 and we are fine.
                    // Therefore, it has to be a multiplication
                    assert( GiNaC::is_exactly_a<GiNaC::mul>(*term) );
                    // Now, we can traverse it and look for a constant part.
                    // As the expression is expanded, we just look for one numeric value.
                    for( GiNaC::const_iterator part = term->begin(); part != term->end(); ++part)
                    {
                        if( GiNaC::is_exactly_a<GiNaC::numeric>(*part))
                        {
                            constPart = constPart.div( GiNaC::ex_to<GiNaC::numeric>(*part) );
                            break;
                        }
                    }

                    if(constPart == (GiNaC::numeric)0 ) continue;

                    if(degree > 2) continue;
                    Formula* deduction = new Formula(OR);

                    switch(constraint->relation())
                    {
                        case CR_LEQ:
                        {
                            addUpperBounds(deduction, constraint->variables(), determineUpperBounds(degree, constPart), false);
                            break;
                        }
                        case CR_GEQ:
                        {
                            #ifdef PREPROCESSING_DEVELOP_MODE
                            assert(false); // not implemented
                            #endif
                            break;
                        }
                        case CR_LESS:
                        {
                            addUpperBounds(deduction, constraint->variables(), determineUpperBounds(degree, constPart), true);
                            break;
                        }
                        case CR_GREATER:
                        {
                            #ifdef PREPROCESSING_DEVELOP_MODE
                            assert(false); // not implemented
                            #endif
                            break;
                        }
                        case CR_EQ:
                        {
                            #ifdef PREPROCESSING_DEVELOP_MODE
                            assert(false); // not implemented
                            #endif
                            break;
                        }
                        case CR_NEQ:
                        {
                            #ifdef PREPROCESSING_DEVELOP_MODE
                            assert(false); // not implemented
                            #endif
                            break;
                        }
                    }
                    formula->addSubformula(deduction);
                }
            }
        }

    }

    void PreprocessingModule::addUpperBounds(Formula* formula, const GiNaC::symtab& symbols, GiNaC::numeric boundary, bool strict  ) const
    {
        for(GiNaC::symtab::const_iterator it = symbols.begin(); it != symbols.end(); ++it )
        {
           GiNaC::ex lhs(it->second - boundary);
           GiNaC::symtab sym;
           sym.insert(*it);
           const Constraint* constraint = newConstraint( lhs, (strict ? CR_LESS : CR_LEQ), sym );
           formula->addSubformula(constraint);
        }
    }

    /**
     * TODO extend this method for more than degree 2
     * Given a constraint at - c lessequal 0 with t being a monomial and a and c constants, we deduce an upper bound for one of the variables each.
     * @return A constant d such that we have (x_1 - d lessequal 0 or ... or x_n - d lessequal 0)
     */
    GiNaC::numeric PreprocessingModule::determineUpperBounds(unsigned degree, const GiNaC::numeric& constPart) const
    {
        assert(degree == 2);
        std::vector<int>::const_iterator upperBound = std::upper_bound (squares.begin(), squares.end(), -constPart);
        if(upperBound != squares.end())
        {
            return upperBound - squares.begin();
        }
        else
        {
            return -constPart;
        }
    }
}


