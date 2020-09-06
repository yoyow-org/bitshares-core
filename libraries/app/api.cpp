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
#include <cctype>

#include <graphene/app/api.hpp>
#include <graphene/app/api_access.hpp>
#include <graphene/app/application.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/get_config.hpp>
#include <graphene/utilities/key_conversion.hpp>
#include <graphene/chain/protocol/fee_schedule.hpp>
#include <graphene/chain/transaction_object.hpp>

#include <fc/crypto/base64.hpp>
#include <fc/crypto/hex.hpp>

namespace graphene { namespace app {

    login_api::login_api(application& a)
    :_app(a)
    {
    }

    login_api::~login_api()
    {
    }

    bool login_api::login(const string& user, const string& password)
    {
       optional< api_access_info > acc = _app.get_api_access_info( user );
       if( !acc.valid() )
          return false;
       if( acc->password_hash_b64 != "*" )
       {
          std::string password_salt = fc::base64_decode( acc->password_salt_b64 );
          std::string acc_password_hash = fc::base64_decode( acc->password_hash_b64 );

          fc::sha256 hash_obj = fc::sha256::hash( password + password_salt );
          if( hash_obj.data_size() != acc_password_hash.length() )
             return false;
          if( memcmp( hash_obj.data(), acc_password_hash.c_str(), hash_obj.data_size() ) != 0 )
             return false;
       }

       for( const std::string& api_name : acc->allowed_apis )
          enable_api( api_name );
       return true;
    }

    void login_api::enable_api( const std::string& api_name )
    {
       if( api_name == "database_api" )
       {
          _database_api = std::make_shared< database_api >(std::ref(*_app.chain_database()), &(_app.get_options()));
       }
       else if( api_name == "block_api" )
       {
          _block_api = std::make_shared< block_api >( std::ref( *_app.chain_database() ) );
       }
       else if( api_name == "network_broadcast_api" )
       {
          _network_broadcast_api = std::make_shared< network_broadcast_api >( std::ref( _app ) );
       }
       else if( api_name == "history_api" )
       {
          _history_api = std::make_shared< history_api >( _app );
       }
       else if( api_name == "network_node_api" )
       {
          _network_node_api = std::make_shared< network_node_api >( std::ref(_app) );
       }
       else if( api_name == "crypto_api" )
       {
          _crypto_api = std::make_shared< crypto_api >();
       }
       else if( api_name == "asset_api" )
       {
          _asset_api = std::make_shared< asset_api >( std::ref( *_app.chain_database() ) );
       }
       else if( api_name == "debug_api" )
       {
          // can only enable this API if the plugin was loaded
          if( _app.get_plugin( "debug_witness" ) )
             _debug_api = std::make_shared< graphene::debug_witness::debug_api >( std::ref(_app) );
       }
       return;
    }

    // block_api
    block_api::block_api(graphene::chain::database& db) : _db(db) { }
    block_api::~block_api() { }

    vector<optional<signed_block>> block_api::get_blocks(uint32_t block_num_from, uint32_t block_num_to)const
    {
       FC_ASSERT( block_num_to >= block_num_from );
       vector<optional<signed_block>> res;
       for(uint32_t block_num=block_num_from; block_num<=block_num_to; block_num++) {
          res.push_back(_db.fetch_block_by_number(block_num));
       }
       return res;
    }

    network_broadcast_api::network_broadcast_api(application& a):_app(a)
    {
       _applied_block_connection = _app.chain_database()->applied_block.connect([this](const signed_block& b){ on_applied_block(b); });
    }

    void network_broadcast_api::on_applied_block( const signed_block& b )
    {
       if( _callbacks.size() )
       {
          /// we need to ensure the database_api is not deleted for the life of the async operation
          auto capture_this = shared_from_this();
          for( uint32_t trx_num = 0; trx_num < b.transactions.size(); ++trx_num )
          {
             const auto& trx = b.transactions[trx_num];
             auto id = trx.id();
             auto itr = _callbacks.find(id);
             if( itr != _callbacks.end() )
             {
                auto block_num = b.block_num();
                auto& callback = _callbacks.find(id)->second;
                auto v = fc::variant( transaction_confirmation{ id, block_num, trx_num, trx }, GRAPHENE_MAX_NESTED_OBJECTS );
                fc::async( [capture_this,v,callback]() {
                   callback(v);
                } );
             }
          }
       }
    }

    void network_broadcast_api::broadcast_transaction(const precomputable_transaction& trx)
    {
       _app.chain_database()->precompute_parallel( trx ).wait();
       _app.chain_database()->push_transaction(trx);
       if( _app.p2p_node() != nullptr )
          _app.p2p_node()->broadcast_transaction(trx);
    }

	void network_broadcast_api::broadcast_transaction_batch(const vector<precomputable_transaction>& trxs)
    {
       _app.chain_database()->precompute_parallel( trxs ).wait();
	   for(const auto& trx:trxs)
	   {
	       _app.chain_database()->push_transaction(trx);
	       if( _app.p2p_node() != nullptr )
	          _app.p2p_node()->broadcast_transaction(trx);
	   }
    }

    fc::variant network_broadcast_api::broadcast_transaction_synchronous(const precomputable_transaction& trx)
    {
       fc::promise<fc::variant>::ptr prom = fc::promise<fc::variant>::create();
       broadcast_transaction_with_callback( [prom]( const fc::variant& v ){
        prom->set_value(v);
       }, trx );

       return fc::future<fc::variant>(prom).wait();
    }

    void network_broadcast_api::broadcast_block( const signed_block& b )
    {
       _app.chain_database()->precompute_parallel( b ).wait();
       _app.chain_database()->push_block(b);
       if( _app.p2p_node() != nullptr )
          _app.p2p_node()->broadcast( net::block_message( b ));
    }

    void network_broadcast_api::broadcast_transaction_with_callback(confirmation_callback cb, const precomputable_transaction& trx)
    {
       _app.chain_database()->precompute_parallel( trx ).wait();
       _callbacks[trx.id()] = cb;
       _app.chain_database()->push_transaction(trx);
       if( _app.p2p_node() != nullptr )
          _app.p2p_node()->broadcast_transaction(trx);
    }

    network_node_api::network_node_api( application& a ) : _app( a )
    {
    }

    fc::variant_object network_node_api::get_info() const
    {
       fc::mutable_variant_object result = _app.p2p_node()->network_get_info();
       result["connection_count"] = _app.p2p_node()->get_connection_count();
       return result;
    }

    void network_node_api::add_node(const fc::ip::endpoint& ep)
    {
       _app.p2p_node()->add_node(ep);
    }

    std::vector<net::peer_status> network_node_api::get_connected_peers() const
    {
       return _app.p2p_node()->get_connected_peers();
    }

    std::vector<net::potential_peer_record> network_node_api::get_potential_peers() const
    {
       return _app.p2p_node()->get_potential_peers();
    }

    fc::variant_object network_node_api::get_advanced_node_parameters() const
    {
       return _app.p2p_node()->get_advanced_node_parameters();
    }

    void network_node_api::set_advanced_node_parameters(const fc::variant_object& params)
    {
       return _app.p2p_node()->set_advanced_node_parameters(params);
    }

    fc::api<network_broadcast_api> login_api::network_broadcast()const
    {
       FC_ASSERT(_network_broadcast_api);
       return *_network_broadcast_api;
    }

    fc::api<block_api> login_api::block()const
    {
       FC_ASSERT(_block_api);
       return *_block_api;
    }

    fc::api<network_node_api> login_api::network_node()const
    {
       FC_ASSERT(_network_node_api);
       return *_network_node_api;
    }

    fc::api<database_api> login_api::database()const
    {
       FC_ASSERT(_database_api);
       return *_database_api;
    }

    fc::api<history_api> login_api::history() const
    {
       FC_ASSERT(_history_api);
       return *_history_api;
    }

    fc::api<crypto_api> login_api::crypto() const
    {
       FC_ASSERT(_crypto_api);
       return *_crypto_api;
    }

    fc::api<asset_api> login_api::asset() const
    {
       FC_ASSERT(_asset_api);
       return *_asset_api;
    }

    fc::api<graphene::debug_witness::debug_api> login_api::debug() const
    {
       FC_ASSERT(_debug_api);
       return *_debug_api;
    }

    vector<operation_history_object> history_api::get_account_history( account_id_type account, 
                                                                       operation_history_id_type stop, 
                                                                       unsigned limit, 
                                                                       operation_history_id_type start ) const
    {
       FC_ASSERT( _app.chain_database() );
       const auto& db = *_app.chain_database();       
       FC_ASSERT( limit <= 100 );
       vector<operation_history_object> result;
       const auto& stats = account(db).statistics(db);
       if( stats.most_recent_op == account_transaction_history_id_type() ) return result;
       const account_transaction_history_object* node = &stats.most_recent_op(db);
       if( start == operation_history_id_type() )
          start = node->operation_id;
          
       while(node && node->operation_id.instance.value > stop.instance.value && result.size() < limit)
       {
          if( node->operation_id.instance.value <= start.instance.value )
             result.push_back( node->operation_id(db) );
          if( node->next == account_transaction_history_id_type() )
             node = nullptr;
          else node = &node->next(db);
       }
       
       return result;
    }
    
    vector<operation_history_object> history_api::get_account_history_operations( account_id_type account, 
                                                                       int operation_id,
                                                                       operation_history_id_type start, 
                                                                       operation_history_id_type stop,
                                                                       unsigned limit) const
    {
       FC_ASSERT( _app.chain_database() );
       const auto& db = *_app.chain_database();
       FC_ASSERT( limit <= 100 );
       vector<operation_history_object> result;
       const auto& stats = account(db).statistics(db);
       if( stats.most_recent_op == account_transaction_history_id_type() ) return result;
       const account_transaction_history_object* node = &stats.most_recent_op(db);
       if( start == operation_history_id_type() )
          start = node->operation_id;

       while(node && node->operation_id.instance.value > stop.instance.value && result.size() < limit)
       {
          if( node->operation_id.instance.value <= start.instance.value ) {

             if(node->operation_id(db).op.which() == operation_id)
               result.push_back( node->operation_id(db) );
             }
          if( node->next == account_transaction_history_id_type() )
             node = nullptr;
          else node = &node->next(db);
       }
       return result;
    }


    vector<std::pair<uint32_t,operation_history_object>> history_api::get_relative_account_history( account_uid_type account,
                                                                                                    optional<uint16_t> op_type,
                                                                                                    uint32_t stop,
                                                                                                    unsigned limit,
                                                                                                    uint32_t start) const
    {
       FC_ASSERT( _app.chain_database() );
       const auto& db = *_app.chain_database();
       FC_ASSERT(limit <= 100);
       vector<std::pair<uint32_t,operation_history_object>> result;
       const auto& stats = db.get_account_statistics_by_uid( account );
       if( start == 0 )
          start = stats.total_ops;
       else
          start = min( stats.total_ops, start );


       if( start >= stop && start > stats.removed_ops && limit > 0 )
       {
          const auto& hist_idx = db.get_index_type<account_transaction_history_index>();

          if( !op_type.valid() )
          {
             const auto& by_seq_idx = hist_idx.indices().get<by_seq>();

             auto itr = by_seq_idx.upper_bound( boost::make_tuple( account, start ) );
             auto itr_stop = by_seq_idx.lower_bound( boost::make_tuple( account, stop ) );
         
             do
             {
                --itr;
                result.push_back( std::make_pair( itr->sequence, itr->operation_id(db) ) );
             }
             while ( itr != itr_stop && result.size() < limit );
          }
          else
          {
             const auto& by_type_seq_idx = hist_idx.indices().get<by_type_seq>();

             auto itr = by_type_seq_idx.upper_bound( boost::make_tuple( account, *op_type, start ) );
             auto itr_stop = by_type_seq_idx.lower_bound( boost::make_tuple( account, *op_type, stop ) );

             if( itr != itr_stop )
             {
                do
                {
                   --itr;
                   result.push_back( std::make_pair( itr->sequence, itr->operation_id(db) ) );
                }
                while ( itr != itr_stop && result.size() < limit );
             }
          }
       }
       return result;
    }

    vector<bucket_object> history_api::get_market_history(std::string asset_a, std::string asset_b,
       uint32_t bucket_seconds, fc::time_point_sec start, fc::time_point_sec end)const
    {
       try {
          FC_ASSERT(_app.chain_database());
          const auto& db = *_app.chain_database();
          asset_aid_type a = database_api.get_asset_id_from_string(asset_a);
          asset_aid_type b = database_api.get_asset_id_from_string(asset_b);
          vector<bucket_object> result;
          result.reserve(200);

          if (a > b) std::swap(a, b);

          const auto& bidx = db.get_index_type<bucket_index>();
          const auto& by_key_idx = bidx.indices().get<by_key>();

          auto itr = by_key_idx.lower_bound(bucket_key(a, b, bucket_seconds, start));
          while (itr != by_key_idx.end() && itr->key.open <= end && result.size() < 200)
          {
             if (!(itr->key.base == a && itr->key.quote == b && itr->key.seconds == bucket_seconds))
             {
                return result;
             }
             result.push_back(*itr);
             ++itr;
          }
          return result;
       } FC_CAPTURE_AND_RETHROW((asset_a)(asset_b)(bucket_seconds)(start)(end))
    }

    vector<order_history_object> history_api::get_fill_order_history(std::string asset_a, std::string asset_b, uint32_t limit)const
    {
       try {
          FC_ASSERT(_app.chain_database());
          const auto& db = *_app.chain_database();
          asset_aid_type a = database_api.get_asset_id_from_string(asset_a);
          asset_aid_type b = database_api.get_asset_id_from_string(asset_b);
          if (a > b) std::swap(a, b);
          const auto& history_idx = db.get_index_type<graphene::market_history::history_index>().indices().get<by_key>();
          history_key hkey;
          hkey.base = a;
          hkey.quote = b;
          hkey.sequence = std::numeric_limits<int64_t>::min();

          uint32_t count = 0;
          auto itr = history_idx.lower_bound(hkey);
          vector<order_history_object> result;
          while (itr != history_idx.end() && count < limit)
          {
             if (itr->key.base != a || itr->key.quote != b) break;
             result.push_back(*itr);
             ++itr;
             ++count;
          }

          return result;
       } FC_CAPTURE_AND_RETHROW((asset_a)(asset_b)(limit))
    }

    crypto_api::crypto_api(){};
                                                               
    commitment_type crypto_api::blind( const blind_factor_type& blind, uint64_t value )
    {
       return fc::ecc::blind( blind, value );
    }
   
    blind_factor_type crypto_api::blind_sum( const std::vector<blind_factor_type>& blinds_in, uint32_t non_neg )
    {
       return fc::ecc::blind_sum( blinds_in, non_neg );
    }
   
    bool crypto_api::verify_sum( const std::vector<commitment_type>& commits_in, const std::vector<commitment_type>& neg_commits_in, int64_t excess )
    {
       return fc::ecc::verify_sum( commits_in, neg_commits_in, excess );
    }
    
    verify_range_result crypto_api::verify_range( const commitment_type& commit, const std::vector<char>& proof )
    {
       verify_range_result result;
       result.success = fc::ecc::verify_range( result.min_val, result.max_val, commit, proof );
       return result;
    }
    
    std::vector<char> crypto_api::range_proof_sign( uint64_t min_value, 
                                                    const commitment_type& commit, 
                                                    const blind_factor_type& commit_blind, 
                                                    const blind_factor_type& nonce,
                                                    int8_t base10_exp,
                                                    uint8_t min_bits,
                                                    uint64_t actual_value )
    {
       return fc::ecc::range_proof_sign( min_value, commit, commit_blind, nonce, base10_exp, min_bits, actual_value );
    }
                               
    verify_range_proof_rewind_result crypto_api::verify_range_proof_rewind( const blind_factor_type& nonce,
                                                                            const commitment_type& commit, 
                                                                            const std::vector<char>& proof )
    {
       verify_range_proof_rewind_result result;
       result.success = fc::ecc::verify_range_proof_rewind( result.blind_out, 
                                                            result.value_out, 
                                                            result.message_out, 
                                                            nonce, 
                                                            result.min_val, 
                                                            result.max_val, 
                                                            const_cast< commitment_type& >( commit ), 
                                                            proof );
       return result;
    }
                                    
    range_proof_info crypto_api::range_get_info( const std::vector<char>& proof )
    {
       return fc::ecc::range_get_info( proof );
    }

    // asset_api
    asset_api::asset_api(graphene::chain::database& db) : _db(db) { }
    asset_api::~asset_api() { }

    vector<account_asset_balance> asset_api::get_asset_holders( asset_aid_type asset_id ) const {

      const auto& bal_idx = _db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
      auto range = bal_idx.equal_range( asset_id );

      vector<account_asset_balance> result;

      for( const account_balance_object& bal : boost::make_iterator_range( range.first, range.second ) )
      {
        if( bal.balance.value == 0 ) break; // ordered

        account_asset_balance aab;
        aab.account_uid = bal.owner;
        aab.amount      = bal.balance.value;

        result.push_back(aab);
      }

      return result;
    }
    // get number of asset holders.
    uint64_t asset_api::get_asset_holders_count( asset_aid_type asset_id ) const {

      const auto& bal_idx = _db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
      auto range = bal_idx.equal_range( asset_id );
      // TODO FIXME filter out accounts with balance == 0
      uint64_t count = boost::distance(range) - 1;

      return count;
    }
    // function to get vector of system assets with holders count.
    vector<asset_holders> asset_api::get_all_asset_holders() const {
            
      vector<asset_holders> result;
            
      for( const asset_object& asset_obj : _db.get_index_type<asset_index>().indices() )
      {
        const auto& bal_idx = _db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
        auto range = bal_idx.equal_range( asset_obj.asset_id );
        // TODO FIXME filter out accounts with balance == 0
        uint64_t count = boost::distance(range) - 1;
                
        asset_holders ah;
        ah.asset_id  = asset_obj.asset_id;
        ah.count     = count;

        result.push_back(ah);
      }

      return result;
    }

} } // graphene::app
