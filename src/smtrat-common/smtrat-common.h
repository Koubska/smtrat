#pragma once

#include "CompileInfo.h"

#include <carl/core/MultivariatePolynomial.h>
#include <carl/core/Variable.h>
#include <carl/formula/Formula.h>
#include <carl/io/streamingOperators.h>

namespace smtrat {

using carl::operator<<;

using Rational = mpq_class;

using Poly = carl::MultivariatePolynomial<Rational>;

using ConstraintT = carl::Constraint<Poly>;

using ConstraintsT = carl::Constraints<Poly>;

using FormulaT = carl::Formula<Poly>;

using FormulasT = carl::Formulas<Poly>;

}