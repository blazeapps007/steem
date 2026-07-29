#ifdef IS_TEST_NET
#include <boost/test/unit_test.hpp>

#include <steem/chain/steem_fwd.hpp>

#include <steem/protocol/exceptions.hpp>
#include <steem/protocol/hardfork.hpp>

#include <steem/chain/database.hpp>
#include <steem/chain/database_exceptions.hpp>
#include <steem/chain/steem_objects.hpp>

#include <fc/crypto/sha256.hpp>

#include "../db_fixture/database_fixture.hpp"

#include <iostream>

using namespace steem;
using namespace steem::chain;
using namespace steem::protocol;
using fc::string;

BOOST_FIXTURE_TEST_SUITE( bridge_oracle_tests, clean_database_fixture )

BOOST_AUTO_TEST_CASE( bridge_submit_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: bridge_submit_validate" );

      bridge_submit_operation op;
      op.publisher = "alice";

      // An empty batch is a valid no-op.
      op.validate();

      bridge_request r;
      r.tx_hash    = fc::sha256::hash( string( "tx1" ) );
      r.block_num  = 100;
      r.block_time = fc::time_point_sec( 1000 );
      r.recipient  = "bob";
      r.amount     = 1000;
      r.symbol     = STEEM_SYMBOL;
      op.requests.push_back( r );
      op.validate();

      // SBD is also valid.
      op.requests[0].symbol = SBD_SYMBOL;
      op.validate();

      // A non-STEEM/SBD symbol is rejected.
      op.requests[0].symbol = VESTS_SYMBOL;
      STEEM_REQUIRE_THROW( op.validate(), fc::assert_exception );
      op.requests[0].symbol = STEEM_SYMBOL;

      // Non-positive amount is rejected.
      op.requests[0].amount = 0;
      STEEM_REQUIRE_THROW( op.validate(), fc::assert_exception );
      op.requests[0].amount = -5;
      STEEM_REQUIRE_THROW( op.validate(), fc::assert_exception );
      op.requests[0].amount = 1000;

      // Invalid recipient name is rejected.
      op.requests[0].recipient = "";
      STEEM_REQUIRE_THROW( op.validate(), fc::assert_exception );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( bridge_submit_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: bridge_submit_authorities (publisher active authority)" );

      bridge_submit_operation op;
      op.publisher = "alice";

      flat_set< account_name_type > auths;
      flat_set< account_name_type > expected;

      op.get_required_owner_authorities( auths );
      BOOST_REQUIRE( auths == expected );

      op.get_required_posting_authorities( auths );
      BOOST_REQUIRE( auths == expected );

      expected.insert( "alice" );
      op.get_required_active_authorities( auths );
      BOOST_REQUIRE( auths == expected );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( bridge_submit_apply_single )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: a single confirmation from a scheduled witness creates a candidate" );

      ACTORS( (bob)(carol) )
      generate_block();

      bridge_request r;
      r.tx_hash    = fc::sha256::hash( string( "txA" ) );
      r.block_num  = 7;
      r.block_time = fc::time_point_sec( 55555 );
      r.recipient  = "bob";
      r.amount     = 1000;
      r.symbol     = STEEM_SYMBOL;

      // initminer is a scheduled witness in the fixture, so its confirmation counts.
      {
         bridge_submit_operation op;
         op.publisher = STEEM_INIT_MINER_NAME;
         op.requests.push_back( r );

         signed_transaction tx;
         tx.set_expiration( db->head_block_time() + STEEM_MAX_TIME_UNTIL_EXPIRATION );
         tx.operations.push_back( op );
         sign( tx, init_account_priv_key );
         db->push_transaction( tx, 0 );
      }

      const auto& idx = db->get_index< bridge_oracle_index >().indices().get< by_tx_hash >();
      auto itr = idx.find( r.tx_hash );
      BOOST_REQUIRE( itr != idx.end() );
      BOOST_REQUIRE_EQUAL( itr->confirmation_count, 1u );
      BOOST_REQUIRE( itr->consensus_block == 0 );      // 1 << 17, not yet at consensus
      BOOST_REQUIRE( itr->recipient == account_name_type( "bob" ) );
      BOOST_REQUIRE( itr->symbol == STEEM_SYMBOL );

      // Re-submitting the same tx from the same witness is ignored (anti-equivocation / dedup).
      {
         bridge_submit_operation op;
         op.publisher = STEEM_INIT_MINER_NAME;
         op.requests.push_back( r );

         signed_transaction tx;
         tx.set_expiration( db->head_block_time() + STEEM_MAX_TIME_UNTIL_EXPIRATION );
         tx.operations.push_back( op );
         sign( tx, init_account_priv_key );
         db->push_transaction( tx, database::skip_transaction_dupe_check );
      }
      BOOST_REQUIRE_EQUAL( db->get_index< bridge_oracle_index >().indices().get< by_tx_hash >().find( r.tx_hash )->confirmation_count, 1u );

      // A submission from a non-scheduled account (carol) is silently ignored (no new candidate).
      {
         bridge_request r2 = r;
         r2.tx_hash = fc::sha256::hash( string( "txB" ) );

         bridge_submit_operation op;
         op.publisher = "carol";
         op.requests.push_back( r2 );

         signed_transaction tx;
         tx.set_expiration( db->head_block_time() + STEEM_MAX_TIME_UNTIL_EXPIRATION );
         tx.operations.push_back( op );
         sign( tx, carol_private_key );
         db->push_transaction( tx, 0 );

         const auto& idx2 = db->get_index< bridge_oracle_index >().indices().get< by_tx_hash >();
         BOOST_REQUIRE( idx2.find( r2.tx_hash ) == idx2.end() );
      }
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( bridge_release_maturity )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: earmarked candidate releases at consensus_block + MATURITY, supply-neutral" );

      ACTORS( (bob) )
      generate_block();

      // Pre-fund the reserve (test setup; mirrors operator pre-funding on mainnet).
      fund( STEEM_BRIDGE_BANK_ACCOUNT, ASSET( "1000.000 TESTS" ) );
      generate_block();

      const auto txh = fc::sha256::hash( string( "txRelease" ) );
      const asset release_amount = ASSET( "10.000 TESTS" );

      const asset bank_before = db->get_balance( STEEM_BRIDGE_BANK_ACCOUNT, STEEM_SYMBOL );
      const asset bob_before  = db->get_balance( db->get_account( "bob" ), STEEM_SYMBOL );
      const asset supply_before = db->get_dynamic_global_properties().current_supply;

      // Simulate consensus reached: earmark (debit reserve) and schedule release at now + MATURITY.
      db_plugin->debug_update( [=]( database& db )
      {
         const uint32_t now = db.head_block_num();
         db.adjust_balance( STEEM_BRIDGE_BANK_ACCOUNT, -release_amount ); // earmark; held in-flight by the object
         db.create< bridge_oracle_object >( [&]( bridge_oracle_object& o )
         {
            o.tx_hash            = txh;
            o.payload_hash       = fc::sha256::hash( string( "payload" ) );
            o.block_num          = 1;
            o.block_time         = fc::time_point_sec( 1 );
            o.recipient          = "bob";
            o.amount             = release_amount.amount;
            o.symbol             = STEEM_SYMBOL;
            o.confirmation_count = STEEM_BRIDGE_ORACLE_MIN_CONFIRMATIONS;
            o.created_block      = now;
            o.expires_block      = now + STEEM_BRIDGE_ORACLE_LIFETIME_BLOCKS;
            o.consensus_block    = now;
            o.release_block      = now + STEEM_BRIDGE_ORACLE_MATURITY_BLOCKS;
         });
      });

      generate_block(); // applies the debug update

      // Still pending before maturity; recipient not yet paid.
      {
         const auto& idx = db->get_index< bridge_oracle_index >().indices().get< by_tx_hash >();
         BOOST_REQUIRE( idx.find( txh ) != idx.end() );
         BOOST_REQUIRE( db->get_balance( db->get_account( "bob" ), STEEM_SYMBOL ) == bob_before );
      }

      // Advance well past the maturity window.
      generate_blocks( STEEM_BRIDGE_ORACLE_MATURITY_BLOCKS + 2 );

      // Candidate gone, replay guard written, recipient credited, supply unchanged end-to-end.
      const auto& oidx = db->get_index< bridge_oracle_index >().indices().get< by_tx_hash >();
      BOOST_REQUIRE( oidx.find( txh ) == oidx.end() );

      const auto& pidx = db->get_index< bridge_processed_index >().indices().get< by_processed_tx_hash >();
      BOOST_REQUIRE( pidx.find( txh ) != pidx.end() );

      BOOST_REQUIRE( db->get_balance( db->get_account( "bob" ), STEEM_SYMBOL ) == bob_before + release_amount );
      BOOST_REQUIRE( db->get_balance( STEEM_BRIDGE_BANK_ACCOUNT, STEEM_SYMBOL ) == bank_before - release_amount );
      BOOST_REQUIRE( db->get_dynamic_global_properties().current_supply == supply_before );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( bridge_expiry_non_terminal )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: a candidate that never reaches consensus expires with no payout and no replay guard" );

      const auto txh = fc::sha256::hash( string( "txExpire" ) );

      db_plugin->debug_update( [=]( database& db )
      {
         const uint32_t now = db.head_block_num();
         db.create< bridge_oracle_object >( [&]( bridge_oracle_object& o )
         {
            o.tx_hash            = txh;
            o.payload_hash       = fc::sha256::hash( string( "payloadExpire" ) );
            o.block_num          = 1;
            o.recipient          = STEEM_INIT_MINER_NAME;
            o.amount             = 1000;
            o.symbol             = STEEM_SYMBOL;
            o.confirmation_count = 3;                 // below the threshold
            o.created_block      = now;
            o.expires_block      = now + 1;           // expires almost immediately
            o.consensus_block    = 0;                 // never reached consensus (no funds earmarked)
            o.release_block      = 0;
         });
      });

      generate_blocks( 3 );

      const auto& oidx = db->get_index< bridge_oracle_index >().indices().get< by_tx_hash >();
      BOOST_REQUIRE( oidx.find( txh ) == oidx.end() );   // expired + removed

      const auto& pidx = db->get_index< bridge_processed_index >().indices().get< by_processed_tx_hash >();
      BOOST_REQUIRE( pidx.find( txh ) == pidx.end() );   // NOT terminal — tx_hash remains re-submittable

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
#endif
