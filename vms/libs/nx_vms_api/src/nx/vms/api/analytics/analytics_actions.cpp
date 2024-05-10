#include "analytics_actions.h"

#include <nx/fusion/model_functions.h>

namespace nx::vms::api {

QN_FUSION_ADAPT_STRUCT_FUNCTIONS(AnalyticsAction, (json), AnalyticsAction_Fields)
QN_FUSION_ADAPT_STRUCT_FUNCTIONS(AnalyticsActionResult, (json), AnalyticsActionResult_Fields)

DeprecatedFieldNames const * AnalyticsAction::getDeprecatedFieldNames()
{
    // Since /rest/v4.
    const static DeprecatedFieldNames names{{"parameters", "params"}};
    return &names;
}

} // namespace nx::vms::api::analytics
