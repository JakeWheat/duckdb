#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/table_star_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

using namespace duckdb_libpgquery;

unique_ptr<ParsedExpression> Transformer::TransformColumnRef(PGColumnRef *root) {
	auto fields = root->fields;
	switch ((reinterpret_cast<PGNode *>(fields->head->data.ptr_value))->type) {
	case T_PGString: {
		if (fields->length < 1) {
			throw ParserException("Unexpected field length");
		}
		string column_name, table_name;
		if (fields->length == 1) {
			column_name = string(reinterpret_cast<PGValue *>(fields->head->data.ptr_value)->val.str);
			auto colref = make_unique<ColumnRefExpression>(column_name, table_name);
			colref->query_location = root->location;
			return move(colref);
		} else if (fields->length == 2) {
			table_name = string(reinterpret_cast<PGValue *>(fields->head->data.ptr_value)->val.str);
			auto col_node = reinterpret_cast<PGNode *>(fields->head->next->data.ptr_value);
			switch (col_node->type) {
			case T_PGString: {
				column_name = string(reinterpret_cast<PGValue *>(col_node)->val.str);
				auto colref = make_unique<ColumnRefExpression>(column_name, table_name);
				colref->query_location = root->location;
				return move(colref);
			}
			case T_PGAStar: {
				return make_unique<TableStarExpression>(table_name);
			}
			default:
				throw NotImplementedException("ColumnRef not implemented!");
			}
		} else {
                    throw NotImplementedException("ColumnRef not implemented!");   
                }
	}
	case T_PGAStar: {
		return make_unique<StarExpression>();
	}
	default:
		break;
	}
	throw NotImplementedException("ColumnRef not implemented!");
}

} // namespace duckdb
