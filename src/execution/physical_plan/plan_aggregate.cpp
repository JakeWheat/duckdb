#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_simple_aggregate.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/aggregate/physical_perfecthash_aggregate.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

static uint32_t RequiredBitsForValue(uint32_t n) {
	idx_t required_bits = 0;
	while (n > 0) {
		n >>= 1;
		required_bits++;
	}
	return required_bits;
}

static bool CanUsePerfectHashAggregate(ClientContext &context, LogicalAggregate &op, vector<idx_t> &bits_per_group) {
	idx_t perfect_hash_bits = 0;
	if (op.group_stats.size() == 0) {
		op.group_stats.resize(op.groups.size());
	}
	for (idx_t group_idx = 0; group_idx < op.groups.size(); group_idx++) {
		auto &group = op.groups[group_idx];
		auto &stats = op.group_stats[group_idx];

		switch (group->return_type.InternalType()) {
		case PhysicalType::INT8:
		case PhysicalType::INT16:
		case PhysicalType::INT32:
		case PhysicalType::INT64:
			break;
		default:
			// we only support simple integer types for perfect hashing
			return false;
		}
		// check if the group has stats available
		auto &group_type = group->return_type;
		if (!stats) {
			// no stats, but we might still be able to use perfect hashing if the type is small enough
			// for small types we can just set the stats to [type_min, type_max]
			switch (group_type.InternalType()) {
			case PhysicalType::INT8:
				stats = make_unique<NumericStatistics>(group_type, Value::MinimumValue(group_type),
				                                       Value::MaximumValue(group_type));
				break;
			case PhysicalType::INT16:
				stats = make_unique<NumericStatistics>(group_type, Value::MinimumValue(group_type),
				                                       Value::MaximumValue(group_type));
				break;
			default:
				// type is too large and there are no stats: skip perfect hashing
				return false;
			}
			// we had no stats before, so we have no clue if there are null values or not
			stats->has_null = true;
		}
		auto &nstats = (NumericStatistics &)*stats;

		if (nstats.min.is_null || nstats.max.is_null) {
			return false;
		}
		// we have a min and a max value for the stats: use that to figure out how many bits we have
		// we add two here, one for the NULL value, and one to make the computation one-indexed
		// (e.g. if min and max are the same, we still need one entry in total)
		int64_t range;
		switch (group_type.InternalType()) {
		case PhysicalType::INT8:
			range = int64_t(nstats.max.GetValueUnsafe<int8_t>()) - int64_t(nstats.min.GetValueUnsafe<int8_t>());
			break;
		case PhysicalType::INT16:
			range = int64_t(nstats.max.GetValueUnsafe<int16_t>()) - int64_t(nstats.min.GetValueUnsafe<int16_t>());
			break;
		case PhysicalType::INT32:
			range = int64_t(nstats.max.GetValueUnsafe<int32_t>()) - int64_t(nstats.min.GetValueUnsafe<int32_t>());
			break;
		case PhysicalType::INT64:
			if (!TrySubtractOperator::Operation(nstats.max.GetValueUnsafe<int64_t>(),
			                                    nstats.min.GetValueUnsafe<int64_t>(), range)) {
				return false;
			}
			break;
		default:
			throw InternalException("Unsupported type for perfect hash (should be caught before)");
		}
		// bail out on any range bigger than 2^32
		if (range >= NumericLimits<int32_t>::Maximum()) {
			return false;
		}
		range += 2;
		// figure out how many bits we need
		idx_t required_bits = RequiredBitsForValue(range);
		bits_per_group.push_back(required_bits);
		perfect_hash_bits += required_bits;
		// check if we have exceeded the bits for the hash
		if (perfect_hash_bits > context.perfect_ht_threshold) {
			// too many bits for perfect hash
			return false;
		}
	}
	for (idx_t i = 0; i < op.expressions.size(); i++) {
		auto &aggregate = (BoundAggregateExpression &)*op.expressions[i];
		if (aggregate.distinct || !aggregate.function.combine) {
			// distinct aggregates are not supported in perfect hash aggregates
			return false;
		}
	}
	return true;
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalAggregate &op) {
	unique_ptr<PhysicalOperator> groupby;
	D_ASSERT(op.children.size() == 1);

	bool all_combinable = true;
	for (idx_t i = 0; i < op.expressions.size(); i++) {
		auto &aggregate = (BoundAggregateExpression &)*op.expressions[i];
		if (!aggregate.function.combine) {
			// unsupported aggregate for simple aggregation: use hash aggregation
			all_combinable = false;
			break;
		}
	}

	auto plan = CreatePlan(*op.children[0]);

	plan = ExtractAggregateExpressions(move(plan), op.expressions, op.groups);

	if (op.groups.size() == 0) {
		// no groups, check if we can use a simple aggregation
		// special case: aggregate entire columns together
		bool use_simple_aggregation = true;
		for (idx_t i = 0; i < op.expressions.size(); i++) {
			auto &aggregate = (BoundAggregateExpression &)*op.expressions[i];
			if (!aggregate.function.simple_update || aggregate.distinct) {
				// unsupported aggregate for simple aggregation: use hash aggregation
				use_simple_aggregation = false;
				break;
			}
		}
		if (use_simple_aggregation) {
			groupby = make_unique_base<PhysicalOperator, PhysicalSimpleAggregate>(op.types, move(op.expressions),
			                                                                      all_combinable);
		} else {
			groupby =
			    make_unique_base<PhysicalOperator, PhysicalHashAggregate>(context, op.types, move(op.expressions));
		}
	} else {
		// groups! create a GROUP BY aggregator
		// use a perfect hash aggregate if possible
		vector<idx_t> required_bits;
		if (CanUsePerfectHashAggregate(context, op, required_bits)) {
			groupby = make_unique_base<PhysicalOperator, PhysicalPerfectHashAggregate>(
			    context, op.types, move(op.expressions), move(op.groups), move(op.group_stats), move(required_bits));
		} else {
			groupby = make_unique_base<PhysicalOperator, PhysicalHashAggregate>(context, op.types, move(op.expressions),
			                                                                    move(op.groups));
		}
	}
	groupby->children.push_back(move(plan));
	return groupby;
}

unique_ptr<PhysicalOperator>
PhysicalPlanGenerator::ExtractAggregateExpressions(unique_ptr<PhysicalOperator> child,
                                                   vector<unique_ptr<Expression>> &aggregates,
                                                   vector<unique_ptr<Expression>> &groups) {
	vector<unique_ptr<Expression>> expressions;
	vector<LogicalType> types;

	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &group = groups[group_idx];
		auto ref = make_unique<BoundReferenceExpression>(group->return_type, expressions.size());
		types.push_back(group->return_type);
		expressions.push_back(move(group));
		groups[group_idx] = move(ref);
	}

	for (auto &aggr : aggregates) {
		auto &bound_aggr = (BoundAggregateExpression &)*aggr;
		for (idx_t child_idx = 0; child_idx < bound_aggr.children.size(); child_idx++) {
			auto &child = bound_aggr.children[child_idx];
			auto ref = make_unique<BoundReferenceExpression>(child->return_type, expressions.size());
			types.push_back(child->return_type);
			expressions.push_back(move(child));
			bound_aggr.children[child_idx] = move(ref);
		}
	}
	if (expressions.empty()) {
		return child;
	}
	auto projection = make_unique<PhysicalProjection>(move(types), move(expressions));
	projection->children.push_back(move(child));
	return move(projection);
}

} // namespace duckdb
