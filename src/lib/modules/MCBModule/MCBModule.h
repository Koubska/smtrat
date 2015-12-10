/**
 * @file MCBModule.h
 * @author Gereon Kremer <gereon.kremer@cs.rwth-aachen.de>
 *
 * Multiple-Choice Blasting
 * Detects arithmetic variabls that allow for a finite number of choices.
 * Blasts there choices to boolean variables.
 *
 * @version 2015-12-10
 * Created on 2015-12-10.
 */

#pragma once

#include "../../solver/PModule.h"
#include "MCBStatistics.h"
#include "MCBSettings.h"

namespace smtrat
{
	template<typename Settings>
	class MCBModule : public PModule
	{
		private:
#ifdef SMTRAT_DEVOPTION_Statistics
			MCBStatistics mStatistics;
#endif
			using Choice = std::tuple<carl::Variable,FormulaT>;
			std::map<Choice, carl::Variable> mChoices;
			
		public:
			typedef Settings SettingsType;
			std::string moduleName() const {
				return SettingsType::moduleName;
			}
			MCBModule(const ModuleInput* _formula, RuntimeSettings* _settings, Conditionals& _conditionals, Manager* _manager = nullptr);

			~MCBModule();
			
			/**
			 * Updates the current assignment into the model.
			 * Note, that this is a unique but possibly symbolic assignment maybe containing newly introduced variables.
			 */
			void updateModel() const;

			/**
			 * Checks the received formula for consistency.
			 * @param _full false, if this module should avoid too expensive procedures and rather return unknown instead.
                         * @param _minimize true, if the module should find an assignment minimizing its objective variable; otherwise any assignment is good.
			 * @return True,	if the received formula is satisfiable;
			 *		 False,   if the received formula is not satisfiable;
			 *		 Unknown, otherwise.
			 */
			Answer checkCore( bool _full = true, bool _minimize = false );
		private:
			void collectBounds(FormulaT::ConstraintBounds& cb, const FormulaT& formula, bool conjunction) const;
			void collectChoices(const FormulaT& formula);
			std::function<void(FormulaT)> collectChoicesFunction;
			FormulaT applyReplacements(const FormulaT& f) const;
	};
}
