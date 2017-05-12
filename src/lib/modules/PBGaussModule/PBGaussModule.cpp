/**
 * @file PBGauss.cpp
 * @author YOUR NAME <YOUR EMAIL ADDRESS>
 *
 * @version 2017-05-03
 * Created on 2017-05-03.
 */

#include "PBGaussModule.h"

namespace smtrat
{
	template<class Settings>
	PBGaussModule<Settings>::PBGaussModule(const ModuleInput* _formula, RuntimeSettings*, Conditionals& _conditionals, Manager* _manager):
		Module( _formula, _conditionals, _manager )
#ifdef SMTRAT_DEVOPTION_Statistics
		, mStatistics(Settings::moduleName)
#endif
	{}
	
	template<class Settings>
	PBGaussModule<Settings>::~PBGaussModule()
	{}
	
	template<class Settings>
	bool PBGaussModule<Settings>::informCore( const FormulaT& _constraint )
	{

		return true;
	}
	
	template<class Settings>
	void PBGaussModule<Settings>::init()
	{}
	
	template<class Settings>
	bool PBGaussModule<Settings>::addCore( ModuleInput::const_iterator _subformula )
	{
		FormulaT f;
		for( const auto& subformula: rReceivedFormula()){
			f = subformula.formula();
			const carl::PBConstraint& c = f.pbConstraint();
			if(c.getRelation() == carl::Relation::EQ){
				equations.push_back(c);
			}else{
				inequalities.push_back(c);
			}
		}
		FormulaT subfA = FormulaT(carl::FormulaType::TRUE);
		FormulaT subfB = FormulaT(carl::FormulaType::TRUE);
		if(equations.size() != 0){
			subfA = gaussAlgorithm();

		}
		else if(inequalities.size() != 0){
		//	subfB = reduce();
		}
		FormulaT formula = FormulaT(carl::FormulaType::AND, subfA, subfB);
		addSubformulaToPassedFormula(formula, _subformula->formula());
		return true;
	}
	
	template<class Settings>
	void PBGaussModule<Settings>::removeCore( ModuleInput::const_iterator _subformula )
	{
		
	}
	
	template<class Settings>
	void PBGaussModule<Settings>::updateModel() const
	{
		mModel.clear();
		if( solverState() == Answer::SAT )
		{
			
		}
	}
	
	template<class Settings>
	Answer PBGaussModule<Settings>::checkCore()
	{
		
		return Answer::UNKNOWN; 
	}

	template<class Settings>
	FormulaT PBGaussModule<Settings>::gaussAlgorithm(){

		//PROBLEM WENN VARIABLE MEHRMALS VORKOMMT!!
		const int rows = equations.size();
		std::vector<double> rhs;

		for(auto it : equations){
			for(auto i : it.getLHS()){
				auto elem = std::find_if(vars.begin(), vars.end(), 
					[&] (const carl::Variable& elem){
						return elem.getName() == i.second.getName();
					});
				if(elem == vars.end()){
					//The variable is not in the list
					vars.push_back(i.second);
				}
			}
			rhs.push_back(it.getRHS());
		}

		
		const int columns = vars.size();

		Eigen::MatrixXd matrix;
		int counter = 0;
		std::vector<double> coef;
		for(auto it : equations){
			auto lhs = it.getLHS();
			auto lhsVars = it.gatherVariables();
			
			for(auto i : vars){
				if(std::find(lhsVars.begin(), lhsVars.end(), i) == lhsVars.end()){
					//Variable is not in the equation ==> coeff must be 0
					coef.push_back(0.0);
				}else{
					auto elem = std::find_if(lhs.begin(), lhs.end(), 
					[&] (const pair<int, carl::Variable>& elem){
						return elem.second == i;
					});
					coef.push_back((double) elem->first);
				}
				counter++;
			}
		}

		std::cout << coef << std::endl;

		matrix = Eigen::MatrixXd::Map(&coef[0], columns, rows).transpose();
		Eigen::VectorXd b = Eigen::VectorXd::Map(&rhs[0], rhs.size());



		int dim;
		if(rows < columns){
			dim = columns;
			Eigen::MatrixXd id(dim, dim);
			Eigen::MatrixXd::Identity(columns,dim);            
			id.setIdentity(columns,dim);
			Eigen::MatrixXd newMatrix(columns, columns);
			id << matrix;
			matrix = id;

			Eigen::VectorXd temp = Eigen::VectorXd::Zero(dim);
			temp << b;
			b = temp;

		}else{
			dim = rows;
		}

		//LU Decomposition

		Eigen::FullPivLU<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic>> lu(matrix);
		Eigen::MatrixXd u(rows, columns);
		Eigen::MatrixXd p(rows, columns);
		Eigen::MatrixXd q(rows, columns);
		Eigen::VectorXd newB;
		Eigen::MatrixXd newUpper;

		u = lu.matrixLU().triangularView<Eigen::Upper>();
		p = lu.permutationP();
		q = lu.permutationQ();
		newB = p * b;
		newUpper = u * q.inverse();

		// Eigen::MatrixXd l(dim, dim);
		// Eigen::MatrixXd::Identity(dim,dim);            
		// l.setIdentity(dim,dim);
		// l.triangularView<Eigen::StrictlyLower>() = lu.matrixLU();

		// std::cout << "Matrix:" << std::endl;
		// std::cout << matrix << std::endl;
		// std::cout << "b:" << std::endl;
		// std::cout << b << std::endl;
		// std::cout << "upper:" << std::endl;
		// std::cout << u << std::endl;
		// std::cout << "permutation P:" << std::endl;
		// std::cout << p << std::endl;
		// std::cout << "permutation Q:" << std::endl;
		// std::cout << q << std::endl;
		// std::cout << "Let us now reconstruct the original matrix m:" << std::endl;
		// std::cout << lu.permutationP().inverse() * l * u * lu.permutationQ().inverse() << std::endl;
		// std::cout << "newB:" << std::endl;
		// std::cout << newB << std::endl;
		// std::cout << "newUpper:" << std::endl;
		// std::cout << newUpper << std::endl;

		return reconstructEqSystem(newUpper, newB);

	}

	template<class Settings>
	FormulaT PBGaussModule<Settings>::reconstructEqSystem(const Eigen::MatrixXd& u, const Eigen::VectorXd& b){
		FormulasT subformulas;
		Eigen::MatrixXd temp = u.transpose();


	}

	template<class Settings>
	FormulaT PBGaussModule<Settings>::reduce(){
		
		for(auto it : inequalities){
			auto iVars = it.gatherVariables();
			carl::Relation rel = it.getRelation();
			for(auto i : equations){
				auto eVars = i.gatherVariables();

				for(int k = 0; k < iVars.size(); k++){
					if(std::find(eVars.begin(), eVars.end(), iVars[k]) != eVars.end()){
						auto iLhs = it.getLHS();
						auto eLhs = i.getLHS();
						if(iLhs[k].first == eLhs[k].first){
							auto newConstraint = addConstraints(it, i, rel);
							equations.erase(equations.begin() + k - 1);
							inequalities.erase(inequalities.begin() + k - 1);
							inequalities.push_back(newConstraint);
						}
					}
				}
			}
		}
	}

	template<class Settings>
	carl::PBConstraint PBGaussModule<Settings>::addConstraints(const carl::PBConstraint& i, const carl::PBConstraint e, carl::Relation rel){

		auto iLHS = i.getLHS();
		auto iRHS = i.getRHS();
		auto iVars = i.gatherVariables();
		auto eLHS = e.getLHS();
		auto eRHS = e.getRHS();
		auto eVars = e.gatherVariables();

		std::vector<std::pair<int, carl::Variable>> newLHS;
		auto newRHS = iRHS + eRHS;


		for(auto it : iLHS){
			newLHS.push_back(std::pair<int, carl::Variable>(it.first, it.second));
		}
		for(auto it : eLHS){
			carl::Variable var = it.second;
			auto elem = std::find_if(newLHS.begin(), newLHS.end(), 
					[&] (const pair<int, carl::Variable>& elem){
						return elem.second == it.second;
					});
			if(elem != newLHS.end()){
				std::pair<int, carl::Variable> b = *elem;
				std::pair<int, carl::Variable> newElem = std::make_pair(it.first + b.first, var);
				newLHS.erase(std::remove(newLHS.begin(), newLHS.end(), b), newLHS.end());
				newLHS.push_back(newElem);
			}
		}

		carl::PBConstraint c;
		c.setLHS(newLHS);
		c.setRelation(rel);
		c.setRHS(newRHS);
		return c;
	}

}

#include "Instantiation.h"
