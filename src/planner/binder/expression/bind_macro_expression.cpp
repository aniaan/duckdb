#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

void ExpressionBinder::ReplaceMacroParametersInLambda(FunctionExpression &function,
                                                      vector<unordered_set<string>> &lambda_params) {

	for (auto &child : function.children) {
		if (child->expression_class != ExpressionClass::LAMBDA) {
			// not a lambda expression
			ReplaceMacroParameters(child, lambda_params);
			continue;
		}

		// special-handling for LHS lambda parameters
		// we do not replace them, and we add them to the lambda_params vector
		auto &lambda_expr = child->Cast<LambdaExpression>();
		string error_message;
		auto column_ref_expressions = lambda_expr.ExtractColumnRefExpressions(error_message);

		if (!error_message.empty()) {
			// possibly a JSON function, replace both LHS and RHS
			ParsedExpressionIterator::EnumerateChildren(*lambda_expr.lhs, [&](unique_ptr<ParsedExpression> &child) {
				ReplaceMacroParameters(child, lambda_params);
			});
			ParsedExpressionIterator::EnumerateChildren(*lambda_expr.expr, [&](unique_ptr<ParsedExpression> &child) {
				ReplaceMacroParameters(child, lambda_params);
			});
			continue;
		}

		// push this level
		lambda_params.emplace_back();

		// push the lambda parameter names
		for (const auto column_ref_expr : column_ref_expressions) {
			auto column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
			lambda_params.back().emplace(column_ref.GetName());
		}

		// only replace in RHS
		ParsedExpressionIterator::EnumerateChildren(*lambda_expr.expr, [&](unique_ptr<ParsedExpression> &child) {
			ReplaceMacroParameters(child, lambda_params);
		});

		// pop this level
		lambda_params.pop_back();
	}
}

void ExpressionBinder::ReplaceMacroParameters(unique_ptr<ParsedExpression> &expr,
                                              vector<unordered_set<string>> &lambda_params) {

	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// if the expression is a parameter, replace it with its argument
		auto &col_ref = expr->Cast<ColumnRefExpression>();

		// don't replace lambda parameters
		if (LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetName())) {
			return;
		}

		bool bind_macro_parameter = false;
		if (col_ref.IsQualified()) {
			if (col_ref.GetTableName().find(DummyBinding::DUMMY_NAME) != string::npos) {
				bind_macro_parameter = true;
			}
		} else {
			bind_macro_parameter = macro_binding->HasMatchingBinding(col_ref.GetColumnName());
		}

		if (bind_macro_parameter) {
			D_ASSERT(macro_binding->HasMatchingBinding(col_ref.GetColumnName()));
			expr = macro_binding->ParamToArg(col_ref);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		// special-handling for lambdas, which are inside function expressions,
		auto &function = expr->Cast<FunctionExpression>();
		if (IsLambdaFunction(function)) {
			// special case
			return ReplaceMacroParametersInLambda(function, lambda_params);
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &sq = (expr->Cast<SubqueryExpression>()).subquery;
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *sq->node, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
		break;
	}
	default: // fall through
		break;
	}

	// replace macro parameters in child expressions
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
}

BindResult ExpressionBinder::BindMacro(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func, idx_t depth,
                                       unique_ptr<ParsedExpression> &expr) {

	// recast function so we can access the scalar member function->expression
	auto &macro_def = macro_func.function->Cast<ScalarMacroFunction>();

	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positionals;
	unordered_map<string, unique_ptr<ParsedExpression>> defaults;

	string error =
	    MacroFunction::ValidateArguments(*macro_func.function, macro_func.name, function, positionals, defaults);
	if (!error.empty()) {
		throw BinderException(binder.FormatError(*expr, error));
	}

	// create a MacroBinding to bind this macro's parameters to its arguments
	vector<LogicalType> types;
	vector<string> names;
	// positional parameters
	for (idx_t i = 0; i < macro_def.parameters.size(); i++) {
		types.emplace_back(LogicalType::SQLNULL);
		auto &param = macro_def.parameters[i]->Cast<ColumnRefExpression>();
		names.push_back(param.GetColumnName());
	}
	// default parameters
	for (auto it = macro_def.default_parameters.begin(); it != macro_def.default_parameters.end(); it++) {
		types.emplace_back(LogicalType::SQLNULL);
		names.push_back(it->first);
		// now push the defaults into the positionals
		positionals.push_back(std::move(defaults[it->first]));
	}
	auto new_macro_binding = make_uniq<DummyBinding>(types, names, macro_func.name);
	new_macro_binding->arguments = &positionals;
	macro_binding = new_macro_binding.get();

	// replace current expression with stored macro expression
	expr = macro_def.expression->Copy();

	// now replace the parameters
	vector<unordered_set<string>> lambda_params;
	ReplaceMacroParameters(expr, lambda_params);

	// bind the unfolded macro
	return BindExpression(expr, depth);
}

} // namespace duckdb
