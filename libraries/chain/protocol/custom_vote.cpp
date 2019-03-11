/*
 * Copyright (c) 2018, YOYOW Foundation PTE. LTD. and contributors.
 */
#include <graphene/chain/protocol/custom_vote.hpp>

#include "../utf8/checked.h"

namespace graphene { namespace chain {

void custom_vote_create_operation::validate()const
{
   validate_op_fee(fee, "custom_vote_create");
   validate_account_uid(create_account, "create_account");

   FC_ASSERT(options.size() > 1 && options.size() < 256, "options size should more than 1 and less than 256");
   FC_ASSERT(minimum_selected_items <= maximum_selected_items,
      "maximum selected items must be grater then  or equal to minimum selected items");
   FC_ASSERT(minimum_selected_items > 0, "minimum selected items must be grater then 0");
   FC_ASSERT(maximum_selected_items < options.size(), "maximum selected items must be less then options size");
   FC_ASSERT(required_asset_amount > 0, "required vote asset amount must be grater then 0");
   
}

share_type custom_vote_create_operation::calculate_fee(const fee_parameters_type& k)const
{
   share_type core_fee_required = k.fee;
   auto hash_size = fc::raw::pack_size(description);
   hash_size += fc::raw::pack_size(options);
   core_fee_required += calculate_data_fee(hash_size, k.price_per_kbyte);
   return core_fee_required;
}

void custom_vote_cast_operation::validate()const
{
   validate_op_fee(fee, "custom_vote_cast ");
   validate_account_uid(voter, "voter");
}

share_type custom_vote_cast_operation::calculate_fee(const fee_parameters_type& k)const
{
   share_type core_fee_required = k.fee;
   return core_fee_required;
}

} } // graphene::chain