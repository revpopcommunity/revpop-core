/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/chain/database.hpp>
#include <graphene/chain/db_with.hpp>

#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/global_property_object.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/htlc_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/proposal_object.hpp>
#include <graphene/chain/ticket_object.hpp>
#include <graphene/chain/transaction_history_object.hpp>
#include <graphene/chain/withdraw_permission_object.hpp>
#include <graphene/chain/witness_object.hpp>

#include <graphene/protocol/fee_schedule.hpp>

namespace graphene { namespace chain {

void database::update_global_dynamic_data( const signed_block& b, const uint32_t missed_blocks )
{
   const dynamic_global_property_object& _dgp = get_dynamic_global_properties();

   // dynamic global properties updating
   modify( _dgp, [&b,this,missed_blocks]( dynamic_global_property_object& dgp ){
      const uint32_t block_num = b.block_num();
      if( BOOST_UNLIKELY( block_num == 1 ) )
         dgp.recently_missed_count = 0;
      else if( _checkpoints.size() && _checkpoints.rbegin()->first >= block_num )
         dgp.recently_missed_count = 0;
      else if( missed_blocks )
         dgp.recently_missed_count += GRAPHENE_RECENTLY_MISSED_COUNT_INCREMENT*missed_blocks;
      else if( dgp.recently_missed_count > GRAPHENE_RECENTLY_MISSED_COUNT_INCREMENT )
         dgp.recently_missed_count -= GRAPHENE_RECENTLY_MISSED_COUNT_DECREMENT;
      else if( dgp.recently_missed_count > 0 )
         dgp.recently_missed_count--;

      dgp.head_block_number = block_num;
      dgp.head_block_id = b.id();
      dgp.time = b.timestamp;
      dgp.current_witness = b.witness;
      dgp.recent_slots_filled = (
           (dgp.recent_slots_filled << 1)
           + 1) << missed_blocks;
      dgp.current_aslot += missed_blocks+1;
   });

   if( !(get_node_properties().skip_flags & skip_undo_history_check) )
   {
      GRAPHENE_ASSERT( _dgp.head_block_number - _dgp.last_irreversible_block_num  < GRAPHENE_MAX_UNDO_HISTORY, undo_database_exception,
                 "The database does not have enough undo history to support a blockchain with so many missed blocks. "
                 "Please add a checkpoint if you would like to continue applying blocks beyond this point.",
                 ("last_irreversible_block_num",_dgp.last_irreversible_block_num)("head", _dgp.head_block_number)
                 ("recently_missed",_dgp.recently_missed_count)("max_undo",GRAPHENE_MAX_UNDO_HISTORY) );
   }

   _undo_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
   _fork_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
}

void database::update_signing_witness(const witness_object& signing_witness, const signed_block& new_block)
{
   const global_property_object& gpo = get_global_properties();
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   uint64_t new_block_aslot = dpo.current_aslot + get_slot_at_time( new_block.timestamp );

   share_type witness_pay = std::min( gpo.parameters.witness_pay_per_block, dpo.witness_budget );

   modify( dpo, [&]( dynamic_global_property_object& _dpo )
   {
      _dpo.witness_budget -= witness_pay;
   } );

   deposit_witness_pay( signing_witness, witness_pay );

   modify( signing_witness, [&]( witness_object& _wit )
   {
      _wit.last_aslot = new_block_aslot;
      _wit.last_confirmed_block_num = new_block.block_num();
   } );
}

void database::update_last_irreversible_block()
{
   const global_property_object& gpo = get_global_properties();
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   // TODO for better performance, move this to db_maint, because only need to do it once per maintenance interval
   vector< const witness_object* > wit_objs;
   wit_objs.reserve( gpo.active_witnesses.size() );
   for( const witness_id_type& wid : gpo.active_witnesses )
      wit_objs.push_back( &(wid(*this)) );

   static_assert( GRAPHENE_IRREVERSIBLE_THRESHOLD > 0, "irreversible threshold must be nonzero" );

   // 1 1 1 2 2 2 2 2 2 2 -> 2     .3*10 = 3
   // 1 1 1 1 1 1 1 2 2 2 -> 1
   // 3 3 3 3 3 3 3 3 3 3 -> 3
   // 3 3 3 4 4 4 4 4 4 4 -> 4

   size_t offset = ((GRAPHENE_100_PERCENT - GRAPHENE_IRREVERSIBLE_THRESHOLD) * wit_objs.size() / GRAPHENE_100_PERCENT);

   std::nth_element( wit_objs.begin(), wit_objs.begin() + offset, wit_objs.end(),
      []( const witness_object* a, const witness_object* b )
      {
         return a->last_confirmed_block_num < b->last_confirmed_block_num;
      } );

   uint32_t new_last_irreversible_block_num = wit_objs[offset]->last_confirmed_block_num;

   if( new_last_irreversible_block_num > dpo.last_irreversible_block_num )
   {
      modify( dpo, [&]( dynamic_global_property_object& _dpo )
      {
         _dpo.last_irreversible_block_num = new_last_irreversible_block_num;
      } );
   }
}

void database::clear_expired_transactions()
{ try {
   //Look for expired transactions in the deduplication list, and remove them.
   //Transactions must have expired by at least two forking windows in order to be removed.
   auto& transaction_idx = static_cast<transaction_index&>(get_mutable_index(implementation_ids,
                                                                             impl_transaction_history_object_type));
   const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
   while( (!dedupe_index.empty()) && (head_block_time() > dedupe_index.begin()->trx.expiration) )
      transaction_idx.remove(*dedupe_index.begin());
} FC_CAPTURE_AND_RETHROW() }

void database::clear_expired_proposals()
{
   const auto& proposal_expiration_index = get_index_type<proposal_index>().indices().get<by_expiration>();
   while( !proposal_expiration_index.empty() && proposal_expiration_index.begin()->expiration_time <= head_block_time() )
   {
      const proposal_object& proposal = *proposal_expiration_index.begin();
      processed_transaction result;
      try {
         if( proposal.is_authorized_to_execute(*this) )
         {
            result = push_proposal(proposal);
            //TODO: Do something with result so plugins can process it.
            continue;
         }
      } catch( const fc::exception& e ) {
         elog("Failed to apply proposed transaction on its expiration. Deleting it.\n${proposal}\n${error}",
              ("proposal", proposal)("error", e.to_detail_string()));
      }
      remove(proposal);
   }
}

/**
 *  let HB = the highest bid for the collateral  (aka who will pay the most DEBT for the least collateral)
 *  let SP = current median feed's Settlement Price 
 *  let LC = the least collateralized call order's swan price (debt/collateral)
 *
 *  If there is no valid price feed or no bids then there is no black swan.
 *
 *  A black swan occurs if MAX(HB,SP) <= LC
 */
bool database::check_for_blackswan( const asset_object& mia, bool enable_black_swan,
                                    const asset_bitasset_data_object* bitasset_ptr )
{
    if( !mia.is_market_issued() ) return false;

    const asset_bitasset_data_object& bitasset = ( bitasset_ptr ? *bitasset_ptr : mia.bitasset_data(*this) );
    if( bitasset.has_settlement() ) return true; // already force settled
    auto settle_price = bitasset.current_feed.settlement_price;
    if( settle_price.is_null() ) return false; // no feed

    const call_order_object* call_ptr = nullptr; // place holder for the call order with least collateral ratio

    asset_id_type debt_asset_id = mia.id;
    auto call_min = price::min( bitasset.options.short_backing_asset, debt_asset_id );

    // check with collateralization
    const auto& call_collateral_index = get_index_type<call_order_index>().indices().get<by_collateral>();
    auto call_itr = call_collateral_index.lower_bound( call_min );
    if( call_itr == call_collateral_index.end() ) // no call order
       return false;
    call_ptr = &(*call_itr);

    if( call_ptr->debt_type() != debt_asset_id ) // no call order
       return false;

    price highest = bitasset.current_feed.max_short_squeeze_price();

    const limit_order_index& limit_index = get_index_type<limit_order_index>();
    const auto& limit_price_index = limit_index.indices().get<by_price>();

    // looking for limit orders selling the most USD for the least CORE
    auto highest_possible_bid = price::max( mia.id, bitasset.options.short_backing_asset );
    // stop when limit orders are selling too little USD for too much CORE
    auto lowest_possible_bid  = price::min( mia.id, bitasset.options.short_backing_asset );

    FC_ASSERT( highest_possible_bid.base.asset_id == lowest_possible_bid.base.asset_id );
    // NOTE limit_price_index is sorted from greatest to least
    auto limit_itr = limit_price_index.lower_bound( highest_possible_bid );
    auto limit_end = limit_price_index.upper_bound( lowest_possible_bid );

    if( limit_itr != limit_end ) {
       FC_ASSERT( highest.base.asset_id == limit_itr->sell_price.base.asset_id );
       highest = std::max( limit_itr->sell_price, highest );
    }

    auto least_collateral = call_ptr->collateralization();
    if( ~least_collateral >= highest  ) 
    {
       wdump( (*call_ptr) );
       elog( "Black Swan detected on asset ${symbol} (${id}) at block ${b}: \n"
             "   Least collateralized call: ${lc}  ${~lc}\n"
           //  "   Highest Bid:               ${hb}  ${~hb}\n"
             "   Settle Price:              ${~sp}  ${sp}\n"
             "   Max:                       ${~h}  ${h}\n",
            ("id",mia.id)("symbol",mia.symbol)("b",head_block_num())
            ("lc",least_collateral.to_real())("~lc",(~least_collateral).to_real())
          //  ("hb",limit_itr->sell_price.to_real())("~hb",(~limit_itr->sell_price).to_real())
            ("sp",settle_price.to_real())("~sp",(~settle_price).to_real())
            ("h",highest.to_real())("~h",(~highest).to_real()) );
       edump((enable_black_swan));
       FC_ASSERT( enable_black_swan, "Black swan was detected during a margin update which is not allowed to trigger a blackswan" );
       if( ~least_collateral <= settle_price )
          // global settle at feed price if possible
          globally_settle_asset(mia, settle_price );
       else
          globally_settle_asset(mia, ~least_collateral );
       return true;
    } 
    return false;
}

void database::clear_expired_orders()
{ try {
         //Cancel expired limit orders
         auto head_time = head_block_time();

         auto& limit_index = get_index_type<limit_order_index>().indices().get<by_expiration>();
         while( !limit_index.empty() && limit_index.begin()->expiration <= head_time )
         {
            const limit_order_object& order = *limit_index.begin();
            cancel_limit_order( order );
         }

   //Process expired force settlement orders
   auto& settlement_index = get_index_type<force_settlement_index>().indices().get<by_expiration>();
   if( !settlement_index.empty() )
   {
      asset_id_type current_asset = settlement_index.begin()->settlement_asset_id();
      asset max_settlement_volume;
      price settlement_fill_price;
      price settlement_price;
      bool current_asset_finished = false;
      bool extra_dump = false;

      auto next_asset = [&current_asset, &current_asset_finished, &settlement_index, &extra_dump] {
         auto bound = settlement_index.upper_bound(current_asset);
         if( bound == settlement_index.end() )
         {
            if( extra_dump )
            {
               ilog( "next_asset() returning false" );
            }
            return false;
         }
         if( extra_dump )
         {
            ilog( "next_asset returning true, bound is ${b}", ("b", *bound) );
         }
         current_asset = bound->settlement_asset_id();
         current_asset_finished = false;
         return true;
      };

      uint32_t count = 0;

      // At each iteration, we either consume the current order and remove it, or we move to the next asset
      for( auto itr = settlement_index.lower_bound(current_asset);
           itr != settlement_index.end();
           itr = settlement_index.lower_bound(current_asset) )
      {
         ++count;
         const force_settlement_object& order = *itr;
         auto order_id = order.id;
         current_asset = order.settlement_asset_id();
         const asset_object& mia_object = get(current_asset);
         const asset_bitasset_data_object& mia = mia_object.bitasset_data(*this);

         extra_dump = ((count >= 1000) && (count <= 1020));

         if( extra_dump )
         {
            wlog( "clear_expired_orders() dumping extra data for iteration ${c}", ("c", count) );
            ilog( "head_block_num is ${hb} current_asset is ${a}", ("hb", head_block_num())("a", current_asset) );
         }

         if( mia.has_settlement() )
         {
            ilog( "Canceling a force settlement because of black swan" );
            cancel_settle_order( order );
            continue;
         }

         // Has this order not reached its settlement date?
         if( order.settlement_date > head_time )
         {
            if( next_asset() )
            {
               if( extra_dump )
               {
                  ilog( "next_asset() returned true when order.settlement_date > head_block_time()" );
               }
               continue;
            }
            break;
         }
         // Can we still settle in this asset?
         if( mia.current_feed.settlement_price.is_null() )
         {
            ilog("Canceling a force settlement in ${asset} because settlement price is null",
                 ("asset", mia_object.symbol));
            cancel_settle_order(order);
            continue;
         }
         if( GRAPHENE_100_PERCENT == mia.options.force_settlement_offset_percent ) // settle something for nothing
         {
            ilog( "Canceling a force settlement in ${asset} because settlement offset is 100%",
                  ("asset", mia_object.symbol));
            cancel_settle_order(order);
            continue;
         }
         if( max_settlement_volume.asset_id != current_asset )
            max_settlement_volume = mia_object.amount(mia.max_force_settlement_volume(mia_object.dynamic_data(*this).current_supply));
         // When current_asset_finished is true, this would be the 2nd time processing the same order.
         // In this case, we move to the next asset.
         if( mia.force_settled_volume >= max_settlement_volume.amount || current_asset_finished )
         {
            /*
            ilog("Skipping force settlement in ${asset}; settled ${settled_volume} / ${max_volume}",
                 ("asset", mia_object.symbol)("settlement_price_null",mia.current_feed.settlement_price.is_null())
                 ("settled_volume", mia.force_settled_volume)("max_volume", max_settlement_volume));
                 */
            if( next_asset() )
            {
               if( extra_dump )
               {
                  ilog( "next_asset() returned true when mia.force_settled_volume >= max_settlement_volume.amount" );
               }
               continue;
            }
            break;
         }

         if( settlement_fill_price.base.asset_id != current_asset ) // only calculate once per asset
            settlement_fill_price = mia.current_feed.settlement_price
                                    / ratio_type( GRAPHENE_100_PERCENT - mia.options.force_settlement_offset_percent,
                                                  GRAPHENE_100_PERCENT );

         if( settlement_price.base.asset_id != current_asset ) // only calculate once per asset
            settlement_price = settlement_fill_price;

         auto& call_index = get_index_type<call_order_index>().indices().get<by_collateral>();
         asset settled = mia_object.amount(mia.force_settled_volume);
         // Match against the least collateralized short until the settlement is finished or we reach max settlements
         while( settled < max_settlement_volume && find_object(order_id) )
         {
            auto itr = call_index.lower_bound(boost::make_tuple(price::min(mia_object.bitasset_data(*this).options.short_backing_asset,
                                                                           mia_object.get_id())));
            // There should always be a call order, since asset exists!
            assert(itr != call_index.end() && itr->debt_type() == mia_object.get_id());
            asset max_settlement = max_settlement_volume - settled;

            if( order.balance.amount == 0 )
            {
               wlog( "0 settlement detected" );
               cancel_settle_order( order );
               break;
            }
            try {
               asset new_settled = match(*itr, order, settlement_price, max_settlement, settlement_fill_price);
               if( new_settled.amount == 0 ) // unable to fill this settle order
               {
                  if( find_object( order_id ) ) // the settle order hasn't been cancelled
                     current_asset_finished = true;
                  break;
               }
               settled += new_settled;
            } 
            catch ( const black_swan_exception& e ) { 
               wlog( "Cancelling a settle_order since it may trigger a black swan: ${o}, ${e}",
                     ("o", order)("e", e.to_detail_string()) );
               cancel_settle_order( order );
               break;
            }
         }
         if( mia.force_settled_volume != settled.amount )
         {
            modify(mia, [settled](asset_bitasset_data_object& b) {
               b.force_settled_volume = settled.amount;
            });
         }
      }
   }
} FC_CAPTURE_AND_RETHROW() }

void database::update_expired_feeds()
{
   const auto head_time = head_block_time();
   const auto next_maint_time = get_dynamic_global_properties().next_maintenance_time;

   const auto& idx = get_index_type<asset_bitasset_data_index>().indices().get<by_feed_expiration>();
   auto itr = idx.begin();
   while( itr != idx.end() && itr->feed_is_expired( head_time ) )
   {
      const asset_bitasset_data_object& b = *itr;
      ++itr; // not always process begin() because old code skipped updating some assets before hf 615
      bool update_cer = false; // for better performance, to only update bitasset once, also check CER in this function
      const asset_object* asset_ptr = nullptr;
      // update feeds, check margin calls
      auto old_median_feed = b.current_feed;
      modify( b, [head_time,next_maint_time,&update_cer]( asset_bitasset_data_object& abdo )
      {
         abdo.update_median_feeds( head_time, next_maint_time );
         if( abdo.need_to_update_cer() )
         {
            update_cer = true;
            abdo.asset_cer_updated = false;
            abdo.feed_cer_updated = false;
         }
      });
      if( !b.current_feed.settlement_price.is_null()
            && !b.current_feed.margin_call_params_equal( old_median_feed ) )
      {
         asset_ptr = &b.asset_id( *this );
         check_call_orders( *asset_ptr, true, false, &b );
      }
      // update CER
      if( update_cer )
      {
         if( !asset_ptr )
            asset_ptr = &b.asset_id( *this );
         if( asset_ptr->options.core_exchange_rate != b.current_feed.core_exchange_rate )
         {
            modify( *asset_ptr, [&b]( asset_object& ao )
            {
               ao.options.core_exchange_rate = b.current_feed.core_exchange_rate;
            });
         }
      }
   } // for each asset whose feed is expired
}

void database::update_core_exchange_rates()
{
   const auto& idx = get_index_type<asset_bitasset_data_index>().indices().get<by_cer_update>();
   if( idx.begin() != idx.end() )
   {
      for( auto itr = idx.rbegin(); itr->need_to_update_cer(); itr = idx.rbegin() )
      {
         const asset_bitasset_data_object& b = *itr;
         const asset_object& a = b.asset_id( *this );
         if( a.options.core_exchange_rate != b.current_feed.core_exchange_rate )
         {
            modify( a, [&b]( asset_object& ao )
            {
               ao.options.core_exchange_rate = b.current_feed.core_exchange_rate;
            });
         }
         modify( b, []( asset_bitasset_data_object& abdo )
         {
            abdo.asset_cer_updated = false;
            abdo.feed_cer_updated = false;
         });
      }
   }
}

void database::update_maintenance_flag( bool new_maintenance_flag )
{
   modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& dpo )
   {
      auto maintenance_flag = dynamic_global_property_object::maintenance_flag;
      dpo.dynamic_flags =
           (dpo.dynamic_flags & ~maintenance_flag)
         | (new_maintenance_flag ? maintenance_flag : 0);
   } );
   return;
}

void database::update_withdraw_permissions()
{
   auto& permit_index = get_index_type<withdraw_permission_index>().indices().get<by_expiration>();
   while( !permit_index.empty() && permit_index.begin()->expiration <= head_block_time() )
      remove(*permit_index.begin());
}

void database::clear_expired_htlcs()
{
   const auto& htlc_idx = get_index_type<htlc_index>().indices().get<by_expiration>();
   while ( htlc_idx.begin() != htlc_idx.end()
         && htlc_idx.begin()->conditions.time_lock.expiration <= head_block_time() )
   {
      const htlc_object& obj = *htlc_idx.begin();
      const auto amount = asset(obj.transfer.amount, obj.transfer.asset_id);
      adjust_balance( obj.transfer.from, amount );
      // notify related parties
      htlc_refund_operation vop( obj.id, obj.transfer.from, obj.transfer.to, amount,
         obj.conditions.hash_lock.preimage_hash, obj.conditions.hash_lock.preimage_size );
      push_applied_operation( vop );
      remove( obj );
   }
}

generic_operation_result database::process_tickets()
{
   generic_operation_result result;
   share_type total_delta_pob;
   share_type total_delta_inactive;
   auto& idx = get_index_type<ticket_index>().indices().get<by_next_update>();
   while( !idx.empty() && idx.begin()->next_auto_update_time <= head_block_time() )
   {
      const ticket_object& ticket = *idx.begin();
      const auto& stat = get_account_stats_by_owner( ticket.account );
      if( ticket.status == withdrawing && ticket.current_type == liquid )
      {
         adjust_balance( ticket.account, ticket.amount );
         // Note: amount.asset_id is checked when creating the ticket, so no check here
         modify( stat, [&ticket](account_statistics_object& aso) {
            aso.total_core_pol -= ticket.amount.amount;
            aso.total_pol_value -= ticket.value;
         });
         result.removed_objects.insert( ticket.id );
         remove( ticket );
      }
      else
      {
         ticket_type old_type = ticket.current_type;
         share_type old_value = ticket.value;
         modify( ticket, []( ticket_object& o ) {
            o.auto_update();
         });
         result.updated_objects.insert( ticket.id );

         share_type delta_inactive_amount;
         share_type delta_forever_amount;
         share_type delta_forever_value;
         share_type delta_other_amount;
         share_type delta_other_value;

         if( old_type == lock_forever ) // It implies that the new type is lock_forever too
         {
            if( ticket.value == 0 )
            {
               total_delta_pob -= ticket.amount.amount;
               total_delta_inactive += ticket.amount.amount;
               delta_inactive_amount = ticket.amount.amount;
               delta_forever_amount = -ticket.amount.amount;
            }
            delta_forever_value = ticket.value - old_value;
         }
         else // old_type != lock_forever
         {
            if( ticket.current_type == lock_forever )
            {
               total_delta_pob += ticket.amount.amount;
               delta_forever_amount = ticket.amount.amount;
               delta_forever_value = ticket.value;
               delta_other_amount = -ticket.amount.amount;
               delta_other_value = -old_value;
            }
            else // ticket.current_type != lock_forever
            {
               delta_other_value = ticket.value - old_value;
            }
         }

         // Note: amount.asset_id is checked when creating the ticket, so no check here
         modify( stat, [delta_inactive_amount,delta_forever_amount,delta_forever_value,
                        delta_other_amount,delta_other_value](account_statistics_object& aso) {
            aso.total_core_inactive += delta_inactive_amount;
            aso.total_core_pob += delta_forever_amount;
            aso.total_core_pol += delta_other_amount;
            aso.total_pob_value += delta_forever_value;
            aso.total_pol_value += delta_other_value;
         });

      }
      // TODO if a lock_forever ticket lost all the value, remove it
   }

   // TODO merge stable tickets with the same account and the same type

   // Update global data
   if( total_delta_pob != 0 || total_delta_inactive != 0 )
   {
      modify( get_dynamic_global_properties(),
              [total_delta_pob,total_delta_inactive]( dynamic_global_property_object& dgp ) {
         dgp.total_pob += total_delta_pob;
         dgp.total_inactive += total_delta_inactive;
      });
   }

   return result;
}

} }
