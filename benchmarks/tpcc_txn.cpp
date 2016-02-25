/*
   Copyright 2015 Rachael Harding

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "tpcc.h"
#include "tpcc_query.h"
#include "tpcc_helper.h"
#include "query.h"
#include "wl.h"
#include "thread.h"
#include "table.h"
#include "row.h"
#include "index_hash.h"
#include "index_btree.h"
#include "tpcc_const.h"
#include "transport.h"
#include "msg_queue.h"
#include "message.h"

void TPCCTxnManager::init(Workload * h_wl) {
	TxnManager::init(h_wl);
	_wl = (TPCCWorkload *) h_wl;
}

RC TPCCTxnManager::run_txn() {
#if MODE == SETUP_MODE
  return RCOK;
#endif
  RC rc = RCOK;
  uint64_t thd_prof_start = get_sys_clock();

#if CC_ALG == CALVIN
  rc = run_calvin_txn();
  return rc;
#endif

  while(rc == RCOK && !is_done()) {
    rc = run_txn_state();
  }

  if(rc == Abort)
    abort();

  INC_STATS(get_thd_id(),thd_prof_wl1,get_sys_clock() - thd_prof_start);

  return rc;

}

bool TPCCTxnManager::is_done() {
  assert(false);
  return false;
}

RC TPCCTxnManager::acquire_locks() {
  assert(CC_ALG == VLL || CC_ALG == CALVIN);
  locking_done = false;
  RC rc = RCOK;
  RC rc2;
  INDEX * index;
  itemid_t * item;
  row_t* row;
  uint64_t key;
  incr_lr();
  TPCCQuery* tpcc_query = (TPCCQuery*) query;

	uint64_t w_id = tpcc_query->w_id;
  uint64_t d_id = tpcc_query->d_id;
  uint64_t c_id = tpcc_query->c_id;
  uint64_t d_w_id = tpcc_query->d_w_id;
  uint64_t c_w_id = tpcc_query->c_w_id;
  uint64_t c_d_id = tpcc_query->c_d_id;
	char * c_last = tpcc_query->c_last;
  uint64_t part_id_w = wh_to_part(w_id);
  uint64_t part_id_c_w = wh_to_part(c_w_id);
  switch(tpcc_query->txn_type) {
    case TPCC_PAYMENT:
      if(GET_NODE_ID(part_id_w) == g_node_id) {
      // WH
        index = _wl->i_warehouse;
        item = index_read(index, w_id, part_id_w);
        row_t * row = ((row_t *)item->location);
        rc2 = get_lock(row,g_wh_update? WR:RD);
        if(rc2 != RCOK)
          rc = rc2;

      // Dist
        key = distKey(d_id, d_w_id);
        item = index_read(_wl->i_district, key, part_id_w);
        row = ((row_t *)item->location);
        rc2 = get_lock(row, WR);
        if(rc2 != RCOK)
          rc = rc2;
      }
      if(GET_NODE_ID(part_id_c_w) == g_node_id) {
      // Cust
        if (tpcc_query->by_last_name) { 

          key = custNPKey(c_last, c_d_id, c_w_id);
          index = _wl->i_customer_last;
          item = index_read(index, key, part_id_c_w);
          int cnt = 0;
          itemid_t * it = item;
          itemid_t * mid = item;
          while (it != NULL) {
            cnt ++;
            it = it->next;
            if (cnt % 2 == 0)
              mid = mid->next;
          }
          row = ((row_t *)mid->location);
          
        }
        else { 
          key = custKey(c_id, c_d_id, c_w_id);
          index = _wl->i_customer_id;
          item = index_read(index, key, part_id_c_w);
          row = (row_t *) item->location;
        }
        rc2  = get_lock(row, WR);
        if(rc2 != RCOK)
          rc = rc2;
 
      }
      break;
    case TPCC_NEW_ORDER:
      if(GET_NODE_ID(part_id_w) == g_node_id) {
      // WH
        index = _wl->i_warehouse;
        item = index_read(index, w_id, part_id_w);
        row_t * row = ((row_t *)item->location);
        rc2 = get_lock(row,RD);
        if(rc2 != RCOK)
          rc = rc2;
      // Cust
        index = _wl->i_customer_id;
        key = custKey(c_id, d_id, w_id);
        item = index_read(index, key, wh_to_part(w_id));
        row = (row_t *) item->location;
        rc2 = get_lock(row, RD);
        if(rc2 != RCOK)
          rc = rc2;
      // Dist
        key = distKey(d_id, w_id);
        item = index_read(_wl->i_district, key, wh_to_part(w_id));
        row = ((row_t *)item->location);
        rc2 = get_lock(row, WR);
        if(rc2 != RCOK)
          rc = rc2;
      }
      // Items
        for(uint64_t i = 0; i < tpcc_query->ol_cnt; i++) {
          if(GET_NODE_ID(wh_to_part(tpcc_query->items[i]->ol_supply_w_id)) != g_node_id) 
            continue;

          key = tpcc_query->items[i]->ol_i_id;
          item = index_read(_wl->i_item, key, 0);
          row = ((row_t *)item->location);
          rc2 = get_lock(row, RD);
          if(rc2 != RCOK)
            rc = rc2;
          key = stockKey(tpcc_query->items[i]->ol_i_id, tpcc_query->items[i]->ol_supply_w_id);
          index = _wl->i_stock;
          item = index_read(index, key, wh_to_part(tpcc_query->items[i]->ol_supply_w_id));
          row = ((row_t *)item->location);
          rc2 = get_lock(row, WR);
          if(rc2 != RCOK)
            rc = rc2;
        }
      break;
    default: assert(false);
  }
  if(decr_lr() == 0) {
    if(ATOM_CAS(lock_ready,false,true))
      rc = RCOK;
  }
  locking_done = true;
  return rc;
}


void TPCCTxnManager::next_tpcc_state() {
  TPCCQuery* tpcc_query = (TPCCQuery*) query;

  switch(state) {
    case TPCC_PAYMENT_S:
      state = TPCC_PAYMENT0;
      break;
    case TPCC_PAYMENT0:
      state = TPCC_PAYMENT1;
      break;
    case TPCC_PAYMENT1:
      state = TPCC_PAYMENT2;
      break;
    case TPCC_PAYMENT2:
      state = TPCC_PAYMENT3;
      break;
    case TPCC_PAYMENT3:
      state = TPCC_PAYMENT4;
      break;
    case TPCC_PAYMENT4:
      state = TPCC_PAYMENT5;
      break;
    case TPCC_PAYMENT5:
      state = TPCC_FIN;
      break;
    case TPCC_NEWORDER_S:
      state = TPCC_NEWORDER0;
      break;
    case TPCC_NEWORDER0:
      state = TPCC_NEWORDER1;
      break;
    case TPCC_NEWORDER1:
      state = TPCC_NEWORDER2;
      break;
    case TPCC_NEWORDER2:
      state = TPCC_NEWORDER3;
      break;
    case TPCC_NEWORDER3:
      state = TPCC_NEWORDER4;
      break;
    case TPCC_NEWORDER4:
      state = TPCC_NEWORDER5;
      break;
    case TPCC_NEWORDER5:
      if(next_item_id < tpcc_query->ol_cnt) {
        state = TPCC_NEWORDER6;
      }
      else {
        state = TPCC_FIN;
      }
      break;
    case TPCC_NEWORDER6: // loop pt 1
      state = TPCC_NEWORDER7;
      break;
    case TPCC_NEWORDER7:
      state = TPCC_NEWORDER8;
      break;
    case TPCC_NEWORDER8: // loop pt 2
      state = TPCC_NEWORDER9;
      break;
    case TPCC_NEWORDER9:
      ++next_item_id;
      if(next_item_id < tpcc_query->ol_cnt) {
        state = TPCC_NEWORDER6;
      }
      else {
        state = TPCC_FIN;
      }
      break;
    case TPCC_FIN:
      break;
    default:
      assert(false);
  }

}

void TPCCTxnManager::send_remote_request() {
  TPCCQuery* tpcc_query = (TPCCQuery*) query;
	uint64_t w_id = tpcc_query->w_id;
  uint64_t c_w_id = tpcc_query->c_w_id;
  TPCCQueryMessage * msg = (TPCCQueryMessage*)Message::create_message(this,RQRY);
  uint64_t dest_node_id = UINT64_MAX;
  if(state == TPCC_PAYMENT0) {
    dest_node_id = GET_NODE_ID(wh_to_part(w_id));
  } else if(state == TPCC_PAYMENT4) {
    dest_node_id = GET_NODE_ID(wh_to_part(c_w_id));
  } else if(state == TPCC_NEWORDER0) {
    dest_node_id = GET_NODE_ID(wh_to_part(w_id));
  } else if(state == TPCC_NEWORDER8) {
    dest_node_id = GET_NODE_ID(wh_to_part(tpcc_query->items[next_item_id]->ol_supply_w_id));
    while(GET_NODE_ID(wh_to_part(tpcc_query->items[next_item_id]->ol_supply_w_id)) != dest_node_id) {
      msg->items.push_back(tpcc_query->items[next_item_id++]);
    }
  } else {
    assert(false);
  }
  msg->state = state;
  msg_queue.enqueue(msg,dest_node_id);
}


RC TPCCTxnManager::run_txn_state() {
  TPCCQuery* tpcc_query = (TPCCQuery*) query;
	uint64_t w_id = tpcc_query->w_id;
  uint64_t d_id = tpcc_query->d_id;
  uint64_t c_id = tpcc_query->c_id;
  uint64_t d_w_id = tpcc_query->d_w_id;
  uint64_t c_w_id = tpcc_query->c_w_id;
  uint64_t c_d_id = tpcc_query->c_d_id;
	char * c_last = tpcc_query->c_last;
  double h_amount = tpcc_query->h_amount;
	bool by_last_name = tpcc_query->by_last_name;
	bool remote = tpcc_query->remote;
	uint64_t ol_cnt = tpcc_query->ol_cnt;
	uint64_t o_entry_d = tpcc_query->o_entry_d;
	uint64_t ol_i_id = tpcc_query->items[next_item_id]->ol_i_id;
	uint64_t ol_supply_w_id = tpcc_query->items[next_item_id]->ol_supply_w_id;
	uint64_t ol_quantity = tpcc_query->items[next_item_id]->ol_quantity;
	uint64_t ol_number = next_item_id;
	uint64_t ol_amount = tpcc_query->ol_amount;
  uint64_t o_id = tpcc_query->o_id;

	uint64_t part_id_w = wh_to_part(w_id);
	uint64_t part_id_c_w = wh_to_part(c_w_id);
  uint64_t part_id_ol_supply_w = wh_to_part(ol_supply_w_id);
  bool w_loc = GET_NODE_ID(part_id_w) == g_node_id;
  bool c_w_loc = GET_NODE_ID(part_id_c_w) == g_node_id;
  bool ol_supply_w_loc = GET_NODE_ID(part_id_ol_supply_w) == g_node_id;

	RC rc = RCOK;

	switch (state) {
		case TPCC_PAYMENT0 :
      if(w_loc)
			  rc = run_payment_0(w_id, d_id, d_w_id, h_amount, row);
      else {
        send_remote_request();
        rc = WAIT_REM;
      }
			break;
		case TPCC_PAYMENT1 :
			rc = run_payment_1(w_id, d_id, d_w_id, h_amount, row);
      break;
		case TPCC_PAYMENT2 :
			rc = run_payment_2(w_id, d_id, d_w_id, h_amount, row);
      break;
		case TPCC_PAYMENT3 :
			rc = run_payment_3(w_id, d_id, d_w_id, h_amount, row);
      break;
		case TPCC_PAYMENT4 :
      if(c_w_loc)
			  rc = run_payment_4( w_id,  d_id, c_id, c_w_id,  c_d_id, c_last, h_amount, by_last_name, row); 
      else {
        send_remote_request();
      }
			break;
		case TPCC_PAYMENT5 :
			rc = run_payment_5( w_id,  d_id, c_id, c_w_id,  c_d_id, c_last, h_amount, by_last_name, row); 
      break;
		case TPCC_NEWORDER0 :
      if(w_loc)
			  rc = new_order_0( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      else {
        send_remote_request();
        rc = WAIT_REM;
      }
			break;
		case TPCC_NEWORDER1 :
			rc = new_order_1( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      break;
		case TPCC_NEWORDER2 :
			rc = new_order_2( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      break;
		case TPCC_NEWORDER3 :
			rc = new_order_3( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      break;
		case TPCC_NEWORDER4 :
			rc = new_order_4( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      break;
		case TPCC_NEWORDER5 :
			rc = new_order_5( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      break;
		case TPCC_NEWORDER6 :
			rc = new_order_6(ol_i_id, row);
			break;
		case TPCC_NEWORDER7 :
			rc = new_order_7(ol_i_id, row);
			break;
		case TPCC_NEWORDER8 :
      if(ol_supply_w_loc) {
			  rc = new_order_8( w_id, d_id, remote, ol_i_id, ol_supply_w_id, ol_quantity,  ol_number, o_id, row); 
      }
      else {
        send_remote_request();
        rc = WAIT_REM;
      }
			break;
		case TPCC_NEWORDER9 :
			rc = new_order_9( w_id, d_id, remote, ol_i_id, ol_supply_w_id, ol_quantity,  ol_number, ol_amount, o_id, row); 
      break;
    case TPCC_FIN :
      state = TPCC_FIN;
      if(tpcc_query->rbk)
        return Abort;
		  //return finish(tpcc_query,false);
      break;
		default:
			assert(false);
	}

  if(rc == RCOK)
    next_tpcc_state();
  return rc;
}

inline RC TPCCTxnManager::run_payment_0(uint64_t w_id, uint64_t d_id, uint64_t d_w_id, double h_amount, row_t *& r_wh_local) {

	uint64_t key;
	itemid_t * item;
/*====================================================+
    	EXEC SQL UPDATE warehouse SET w_ytd = w_ytd + :h_amount
		WHERE w_id=:w_id;
	+====================================================*/
	/*===================================================================+
		EXEC SQL SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name
		INTO :w_street_1, :w_street_2, :w_city, :w_state, :w_zip, :w_name
		FROM warehouse
		WHERE w_id=:w_id;
	+===================================================================*/


  RC rc;
	key = w_id;
	INDEX * index = _wl->i_warehouse; 
	item = index_read(index, key, wh_to_part(w_id));
	assert(item != NULL);
	row_t * r_wh = ((row_t *)item->location);
	if (g_wh_update)
		rc = get_row(r_wh, WR, r_wh_local);
	else 
		rc = get_row(r_wh, RD, r_wh_local);

  if(rc == WAIT)
    INC_STATS_ARR(0,w_cflt,key);
  if(rc == Abort)
    INC_STATS_ARR(0,w_abrt,key);
  return rc;
}

inline RC TPCCTxnManager::run_payment_1(uint64_t w_id, uint64_t d_id, uint64_t d_w_id, double h_amount, row_t * r_wh_local) {

  assert(r_wh_local != NULL);
/*====================================================+
    	EXEC SQL UPDATE warehouse SET w_ytd = w_ytd + :h_amount
		WHERE w_id=:w_id;
	+====================================================*/
	/*===================================================================+
		EXEC SQL SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name
		INTO :w_street_1, :w_street_2, :w_city, :w_state, :w_zip, :w_name
		FROM warehouse
		WHERE w_id=:w_id;
	+===================================================================*/


	double w_ytd;
	r_wh_local->get_value(W_YTD, w_ytd);
	if (g_wh_update) {
		r_wh_local->set_value(W_YTD, w_ytd + h_amount);
	}
  return RCOK;
}

inline RC TPCCTxnManager::run_payment_2(uint64_t w_id, uint64_t d_id, uint64_t d_w_id, double h_amount, row_t *& r_dist_local) {
	/*=====================================================+
		EXEC SQL UPDATE district SET d_ytd = d_ytd + :h_amount
		WHERE d_w_id=:w_id AND d_id=:d_id;
	+=====================================================*/
	uint64_t key;
	itemid_t * item;
	key = distKey(d_id, d_w_id);
	item = index_read(_wl->i_district, key, wh_to_part(w_id));
	assert(item != NULL);
	row_t * r_dist = ((row_t *)item->location);
	RC rc = get_row(r_dist, WR, r_dist_local);
  if(rc == WAIT)
    INC_STATS_ARR(0,d_cflt,key);
  if(rc == Abort)
    INC_STATS_ARR(0,d_abrt,key);
  return rc;

}

inline RC TPCCTxnManager::run_payment_3(uint64_t w_id, uint64_t d_id, uint64_t d_w_id, double h_amount, row_t * r_dist_local) {
  assert(r_dist_local != NULL);

	/*=====================================================+
		EXEC SQL UPDATE district SET d_ytd = d_ytd + :h_amount
		WHERE d_w_id=:w_id AND d_id=:d_id;
	+=====================================================*/
	double d_ytd;
	r_dist_local->get_value(D_YTD, d_ytd);
	r_dist_local->set_value(D_YTD, d_ytd + h_amount);

	return RCOK;
}

inline RC TPCCTxnManager::run_payment_4(uint64_t w_id, uint64_t d_id,uint64_t c_id,uint64_t c_w_id, uint64_t c_d_id, char * c_last, double h_amount, bool by_last_name, row_t *& r_cust_local) { 
	/*====================================================================+
		EXEC SQL SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name
		INTO :d_street_1, :d_street_2, :d_city, :d_state, :d_zip, :d_name
		FROM district
		WHERE d_w_id=:w_id AND d_id=:d_id;
	+====================================================================*/

	itemid_t * item;
	uint64_t key;
	row_t * r_cust;
	if (by_last_name) { 
		/*==========================================================+
			EXEC SQL SELECT count(c_id) INTO :namecnt
			FROM customer
			WHERE c_last=:c_last AND c_d_id=:c_d_id AND c_w_id=:c_w_id;
		+==========================================================*/
		/*==========================================================================+
			EXEC SQL DECLARE c_byname CURSOR FOR
			SELECT c_first, c_middle, c_id, c_street_1, c_street_2, c_city, c_state,
			c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since
			FROM customer
			WHERE c_w_id=:c_w_id AND c_d_id=:c_d_id AND c_last=:c_last
			ORDER BY c_first;
			EXEC SQL OPEN c_byname;
		+===========================================================================*/

		key = custNPKey(c_last, c_d_id, c_w_id);
		// XXX: the list is not sorted. But let's assume it's sorted... 
		// The performance won't be much different.
		INDEX * index = _wl->i_customer_last;
		item = index_read(index, key, wh_to_part(c_w_id));
		assert(item != NULL);
		
		int cnt = 0;
		itemid_t * it = item;
		itemid_t * mid = item;
		while (it != NULL) {
			cnt ++;
			it = it->next;
			if (cnt % 2 == 0)
				mid = mid->next;
		}
		r_cust = ((row_t *)mid->location);
		
		/*============================================================================+
			for (n=0; n<namecnt/2; n++) {
				EXEC SQL FETCH c_byname
				INTO :c_first, :c_middle, :c_id,
					 :c_street_1, :c_street_2, :c_city, :c_state, :c_zip,
					 :c_phone, :c_credit, :c_credit_lim, :c_discount, :c_balance, :c_since;
			}
			EXEC SQL CLOSE c_byname;
		+=============================================================================*/
		// XXX: we don't retrieve all the info, just the tuple we are interested in
	}

	else { // search customers by cust_id
		/*=====================================================================+
			EXEC SQL SELECT c_first, c_middle, c_last, c_street_1, c_street_2,
			c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim,
			c_discount, c_balance, c_since
			INTO :c_first, :c_middle, :c_last, :c_street_1, :c_street_2,
			:c_city, :c_state, :c_zip, :c_phone, :c_credit, :c_credit_lim,
			:c_discount, :c_balance, :c_since
			FROM customer
			WHERE c_w_id=:c_w_id AND c_d_id=:c_d_id AND c_id=:c_id;
		+======================================================================*/
		key = custKey(c_id, c_d_id, c_w_id);
		INDEX * index = _wl->i_customer_id;
		item = index_read(index, key, wh_to_part(c_w_id));
		assert(item != NULL);
		r_cust = (row_t *) item->location;
	}

  	/*======================================================================+
	   	EXEC SQL UPDATE customer SET c_balance = :c_balance, c_data = :c_new_data
   		WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_id = :c_id;
   	+======================================================================*/
	RC rc  = get_row(r_cust, WR, r_cust_local);
  if(rc == WAIT) {
    if(by_last_name) {
      INC_STATS_ARR(0,cnp_cflt,key);
    } else
      INC_STATS_ARR(0,c_cflt,key);
  }
  if(rc == Abort) {
    if(by_last_name) {
      INC_STATS_ARR(0,cnp_abrt,key);
    } else
      INC_STATS_ARR(0,c_abrt,key);
  }

  return rc;
}


inline RC TPCCTxnManager::run_payment_5(uint64_t w_id, uint64_t d_id,uint64_t c_id,uint64_t c_w_id, uint64_t c_d_id, char * c_last, double h_amount, bool by_last_name, row_t * r_cust_local) { 
  assert(r_cust_local != NULL);
	double c_balance;
	double c_ytd_payment;
	double c_payment_cnt;

	r_cust_local->get_value(C_BALANCE, c_balance);
	r_cust_local->set_value(C_BALANCE, c_balance - h_amount);
	r_cust_local->get_value(C_YTD_PAYMENT, c_ytd_payment);
	r_cust_local->set_value(C_YTD_PAYMENT, c_ytd_payment + h_amount);
	r_cust_local->get_value(C_PAYMENT_CNT, c_payment_cnt);
	r_cust_local->set_value(C_PAYMENT_CNT, c_payment_cnt + 1);

	// FIXME? c_credit not used
	//char * c_credit = r_cust_local->get_value(C_CREDIT);

	/*=============================================================================+
	  EXEC SQL INSERT INTO
	  history (h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data)
	  VALUES (:c_d_id, :c_w_id, :c_id, :d_id, :w_id, :datetime, :h_amount, :h_data);
	  +=============================================================================*/
	row_t * r_hist;
	uint64_t row_id;
	// Which partition should we be inserting into?
	_wl->t_history->get_new_row(r_hist, wh_to_part(c_w_id), row_id);
	r_hist->set_value(H_C_ID, c_id);
	r_hist->set_value(H_C_D_ID, c_d_id);
	r_hist->set_value(H_C_W_ID, c_w_id);
	r_hist->set_value(H_D_ID, d_id);
	r_hist->set_value(H_W_ID, w_id);
	int64_t date = 2013;		
	r_hist->set_value(H_DATE, date);
	r_hist->set_value(H_AMOUNT, h_amount);
	insert_row(r_hist, _wl->t_history);

	return RCOK;
}



// new_order 0
inline RC TPCCTxnManager::new_order_0(uint64_t w_id, uint64_t d_id, uint64_t c_id, bool remote, uint64_t  ol_cnt,uint64_t  o_entry_d, uint64_t * o_id, row_t *& r_wh_local) {
	uint64_t key;
	itemid_t * item;
	/*=======================================================================+
	EXEC SQL SELECT c_discount, c_last, c_credit, w_tax
		INTO :c_discount, :c_last, :c_credit, :w_tax
		FROM customer, warehouse
		WHERE w_id = :w_id AND c_w_id = w_id AND c_d_id = :d_id AND c_id = :c_id;
	+========================================================================*/
	key = w_id;
	INDEX * index = _wl->i_warehouse; 
	item = index_read(index, key, wh_to_part(w_id));
	assert(item != NULL);
	row_t * r_wh = ((row_t *)item->location);
  RC rc = get_row(r_wh, RD, r_wh_local);
  if(rc == WAIT)
    INC_STATS_ARR(0,w_cflt,key);
  if(rc == Abort)
    INC_STATS_ARR(0,w_abrt,key);
  return rc;
}

inline RC TPCCTxnManager::new_order_1(uint64_t w_id, uint64_t d_id, uint64_t c_id, bool remote, uint64_t  ol_cnt,uint64_t  o_entry_d, uint64_t * o_id, row_t * r_wh_local) {
  assert(r_wh_local != NULL);
	double w_tax;
	r_wh_local->get_value(W_TAX, w_tax); 
  return RCOK;
}

inline RC TPCCTxnManager::new_order_2(uint64_t w_id, uint64_t d_id, uint64_t c_id, bool remote, uint64_t  ol_cnt,uint64_t  o_entry_d, uint64_t * o_id, row_t *& r_cust_local) {
	uint64_t key;
	itemid_t * item;
	key = custKey(c_id, d_id, w_id);
	INDEX * index = _wl->i_customer_id;
	item = index_read(index, key, wh_to_part(w_id));
	assert(item != NULL);
	row_t * r_cust = (row_t *) item->location;
  RC rc = get_row(r_cust, RD, r_cust_local);
  if(rc == WAIT)
    INC_STATS_ARR(0,c_cflt,key);
  if(rc == Abort)
    INC_STATS_ARR(0,c_abrt,key);
  return rc;
}

inline RC TPCCTxnManager::new_order_3(uint64_t w_id, uint64_t d_id, uint64_t c_id, bool remote, uint64_t  ol_cnt,uint64_t  o_entry_d, uint64_t * o_id, row_t * r_cust_local) {
  assert(r_cust_local != NULL);
	uint64_t c_discount;
	//char * c_last;
	//char * c_credit;
	r_cust_local->get_value(C_DISCOUNT, c_discount);
	//c_last = r_cust_local->get_value(C_LAST);
	//c_credit = r_cust_local->get_value(C_CREDIT);
  return RCOK;
}
 	
inline RC TPCCTxnManager::new_order_4(uint64_t w_id, uint64_t d_id, uint64_t c_id, bool remote, uint64_t  ol_cnt,uint64_t  o_entry_d, uint64_t * o_id, row_t *& r_dist_local) {
	uint64_t key;
	itemid_t * item;
	/*==================================================+
	EXEC SQL SELECT d_next_o_id, d_tax
		INTO :d_next_o_id, :d_tax
		FROM district WHERE d_id = :d_id AND d_w_id = :w_id;
	EXEC SQL UPDATE d istrict SET d _next_o_id = :d _next_o_id + 1
		WH ERE d _id = :d_id AN D d _w _id = :w _id ;
	+===================================================*/
	key = distKey(d_id, w_id);
	item = index_read(_wl->i_district, key, wh_to_part(w_id));
	assert(item != NULL);
	row_t * r_dist = ((row_t *)item->location);
  RC rc = get_row(r_dist, WR, r_dist_local);
  if(rc == WAIT)
    INC_STATS_ARR(0,d_cflt,key);
  if(rc == Abort) {
    INC_STATS_ARR(0,d_abrt,key);
  }
  return rc;
}

inline RC TPCCTxnManager::new_order_5(uint64_t w_id, uint64_t d_id, uint64_t c_id, bool remote, uint64_t  ol_cnt,uint64_t  o_entry_d, uint64_t * o_id, row_t * r_dist_local) {
  assert(r_dist_local != NULL);
	//double d_tax;
	//int64_t o_id;
	//d_tax = *(double *) r_dist_local->get_value(D_TAX);
	*o_id = *(int64_t *) r_dist_local->get_value(D_NEXT_O_ID);
	(*o_id) ++;
	r_dist_local->set_value(D_NEXT_O_ID, *o_id);

	// return o_id
	/*========================================================================================+
	EXEC SQL INSERT INTO ORDERS (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local)
		VALUES (:o_id, :d_id, :w_id, :c_id, :datetime, :o_ol_cnt, :o_all_local);
	+========================================================================================*/
	row_t * r_order;
	uint64_t row_id;
	_wl->t_order->get_new_row(r_order, wh_to_part(w_id), row_id);
	r_order->set_value(O_ID, *o_id);
	r_order->set_value(O_C_ID, c_id);
	r_order->set_value(O_D_ID, d_id);
	r_order->set_value(O_W_ID, w_id);
	r_order->set_value(O_ENTRY_D, o_entry_d);
	r_order->set_value(O_OL_CNT, ol_cnt);
	int64_t all_local = (remote? 0 : 1);
	r_order->set_value(O_ALL_LOCAL, all_local);
	insert_row(r_order, _wl->t_order);
	/*=======================================================+
    EXEC SQL INSERT INTO NEW_ORDER (no_o_id, no_d_id, no_w_id)
        VALUES (:o_id, :d_id, :w_id);
    +=======================================================*/
	row_t * r_no;
	_wl->t_neworder->get_new_row(r_no, wh_to_part(w_id), row_id);
	r_no->set_value(NO_O_ID, *o_id);
	r_no->set_value(NO_D_ID, d_id);
	r_no->set_value(NO_W_ID, w_id);
	insert_row(r_no, _wl->t_neworder);

	return RCOK;
}



// new_order 1
// Read from replicated read-only item table
inline RC TPCCTxnManager::new_order_6(uint64_t ol_i_id, row_t *& r_item_local) {
		uint64_t key;
		itemid_t * item;
		/*===========================================+
		EXEC SQL SELECT i_price, i_name , i_data
			INTO :i_price, :i_name, :i_data
			FROM item
			WHERE i_id = :ol_i_id;
		+===========================================*/
		key = ol_i_id;
		item = index_read(_wl->i_item, key, 0);
		assert(item != NULL);
		row_t * r_item = ((row_t *)item->location);

    RC rc = get_row(r_item, RD, r_item_local);
    if(rc == WAIT)
      INC_STATS_ARR(0,ol_cflt,key);
    if(rc == Abort)
      INC_STATS_ARR(0,ol_abrt,key);
    return rc;
}

inline RC TPCCTxnManager::new_order_7(uint64_t ol_i_id, row_t * r_item_local) {
  assert(r_item_local != NULL);
		int64_t i_price;
		//char * i_name;
		//char * i_data;
		
		r_item_local->get_value(I_PRICE, i_price);
		//i_name = r_item_local->get_value(I_NAME);
		//i_data = r_item_local->get_value(I_DATA);


	return RCOK;
}

// new_order 2
inline RC TPCCTxnManager::new_order_8(uint64_t w_id,uint64_t  d_id,bool remote, uint64_t ol_i_id, uint64_t ol_supply_w_id, uint64_t ol_quantity,uint64_t  ol_number,uint64_t  o_id, row_t *& r_stock_local) {
		uint64_t key;
		itemid_t * item;

		/*===================================================================+
		EXEC SQL SELECT s_quantity, s_data,
				s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
				s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10
			INTO :s_quantity, :s_data,
				:s_dist_01, :s_dist_02, :s_dist_03, :s_dist_04, :s_dist_05,
				:s_dist_06, :s_dist_07, :s_dist_08, :s_dist_09, :s_dist_10
			FROM stock
			WHERE s_i_id = :ol_i_id AND s_w_id = :ol_supply_w_id;
		EXEC SQL UPDATE stock SET s_quantity = :s_quantity
			WHERE s_i_id = :ol_i_id
			AND s_w_id = :ol_supply_w_id;
		+===============================================*/

		key = stockKey(ol_i_id, ol_supply_w_id);
		INDEX * index = _wl->i_stock;
		item = index_read(index, key, wh_to_part(ol_supply_w_id));
		assert(item != NULL);
		row_t * r_stock = ((row_t *)item->location);
    RC rc = get_row(r_stock, WR, r_stock_local);
    if(rc == WAIT)
      INC_STATS_ARR(0,s_cflt,key);
    if(rc == Abort)
      INC_STATS_ARR(0,s_abrt,key);
    return rc;
}
		
inline RC TPCCTxnManager::new_order_9(uint64_t w_id,uint64_t  d_id,bool remote, uint64_t ol_i_id, uint64_t ol_supply_w_id, uint64_t ol_quantity,uint64_t  ol_number, uint64_t ol_amount, uint64_t  o_id, row_t * r_stock_local) {
  assert(r_stock_local != NULL);
		// XXX s_dist_xx are not retrieved.
		UInt64 s_quantity;
		int64_t s_remote_cnt;
		s_quantity = *(int64_t *)r_stock_local->get_value(S_QUANTITY);
#if !TPCC_SMALL
		int64_t s_ytd;
		int64_t s_order_cnt;
		char * s_data __attribute__ ((unused));
		r_stock_local->get_value(S_YTD, s_ytd);
		r_stock_local->set_value(S_YTD, s_ytd + ol_quantity);
    // In Coordination Avoidance, this record must be protected!
		r_stock_local->get_value(S_ORDER_CNT, s_order_cnt);
		r_stock_local->set_value(S_ORDER_CNT, s_order_cnt + 1);
		s_data = r_stock_local->get_value(S_DATA);
#endif
		if (remote) {
			s_remote_cnt = *(int64_t*)r_stock_local->get_value(S_REMOTE_CNT);
			s_remote_cnt ++;
			r_stock_local->set_value(S_REMOTE_CNT, &s_remote_cnt);
		}
		uint64_t quantity;
		if (s_quantity > ol_quantity + 10) {
			quantity = s_quantity - ol_quantity;
		} else {
			quantity = s_quantity - ol_quantity + 91;
		}
		r_stock_local->set_value(S_QUANTITY, &quantity);

		/*====================================================+
		EXEC SQL INSERT
			INTO order_line(ol_o_id, ol_d_id, ol_w_id, ol_number,
				ol_i_id, ol_supply_w_id,
				ol_quantity, ol_amount, ol_dist_info)
			VALUES(:o_id, :d_id, :w_id, :ol_number,
				:ol_i_id, :ol_supply_w_id,
				:ol_quantity, :ol_amount, :ol_dist_info);
		+====================================================*/
		row_t * r_ol;
		uint64_t row_id;
		_wl->t_orderline->get_new_row(r_ol, wh_to_part(ol_supply_w_id), row_id);
		r_ol->set_value(OL_O_ID, &o_id);
		r_ol->set_value(OL_D_ID, &d_id);
		r_ol->set_value(OL_W_ID, &w_id);
		r_ol->set_value(OL_NUMBER, &ol_number);
		r_ol->set_value(OL_I_ID, &ol_i_id);
#if !TPCC_SMALL
		r_ol->set_value(OL_SUPPLY_W_ID, &ol_supply_w_id);
		r_ol->set_value(OL_QUANTITY, &ol_quantity);
		r_ol->set_value(OL_AMOUNT, &ol_amount);
#endif		
		insert_row(r_ol, _wl->t_orderline);

	return RCOK;
}


RC TPCCTxnManager::run_calvin_txn() {
  TPCCQuery* tpcc_query = (TPCCQuery*) query;
  uint64_t participant_cnt;
  uint64_t active_cnt;
  uint64_t participant_nodes[g_node_cnt];
  uint64_t active_nodes[g_node_cnt];
  RC rc = RCOK;
  uint64_t home_wh_node;
  while(rc == RCOK && this->phase < 6) {
    switch(this->phase) {
      case 1:
        // Phase 1: Read/write set analysis
        participant_cnt = 0;
        active_cnt = 0;
        for(uint64_t i = 0; i < g_node_cnt; i++) {
          participant_nodes[i] = false;
          active_nodes[i] = false;
        }
        home_wh_node = GET_NODE_ID(wh_to_part(tpcc_query->w_id));
        participant_nodes[home_wh_node] = true;
        active_nodes[home_wh_node] = true;
        participant_cnt++;
        active_cnt++;
        if(tpcc_query->txn_type == TPCC_PAYMENT) {
            uint64_t req_nid = GET_NODE_ID(wh_to_part(tpcc_query->c_w_id));
            if(!participant_nodes[req_nid]) {
              participant_cnt++;
              participant_nodes[req_nid] = true;
              active_cnt++;
              active_nodes[req_nid] = true;
            }

        } else if (tpcc_query->txn_type == TPCC_NEW_ORDER) {
          for(uint64_t i = 0; i < tpcc_query->ol_cnt; i++) {
            uint64_t req_nid = GET_NODE_ID(wh_to_part(tpcc_query->items[i]->ol_supply_w_id));
            if(!participant_nodes[req_nid]) {
              participant_cnt++;
              participant_nodes[req_nid] = true;
              active_cnt++;
              active_nodes[req_nid] = true;
            }
          }
        }
#if DEBUG_DISTR
        printf("REQ (%ld): %ld %ld; %ld %ld\n"
            ,get_txn_id()//,tpcc_query->batch_id
            ,participant_nodes[0]
            ,active_nodes[0]
            ,participant_nodes[1]
            ,active_nodes[1]
            );
#endif

        ATOM_ADD(this->phase,1); //2
        break;
      case 2:
        // Phase 2: Perform local reads
        rc = run_tpcc_phase2();
        //release_read_locks(tpcc_query);

        ATOM_ADD(this->phase,1); //3
        break;
      case 3:
        // Phase 3: Serve remote reads
        rc = send_remote_reads(tpcc_query);
        if(active_nodes[g_node_id]) {
          ATOM_ADD(this->phase,1); //4
          if(get_rsp_cnt() == participant_cnt-1) {
            rc = RCOK;
          } else {
            //DEBUG("Phase4 (%ld,%ld)\n",tpcc_query->txn_id,tpcc_query->batch_id);
            rc = WAIT;
          }
        } else { // Done
          rc = RCOK;
          ATOM_ADD(this->phase,3); //6
        }
        break;
      case 4:
        // Phase 4: Collect remote reads
        ATOM_ADD(this->phase,1); //5
        break;
      case 5:
        // Phase 5: Execute transaction / perform local writes
        rc = run_tpcc_phase5();
        rc = calvin_finish(tpcc_query);
        ATOM_ADD(this->phase,1); //6
        //FIXME
        /*
        if(get_rsp2_cnt() == active_cnt-1) {
          rc = RCOK;
        } else {
        //DEBUG("Phase6 (%ld,%ld)\n",tpcc_query->txn_id,tpcc_query->batch_id);
            rc = WAIT;
        }
        */
        break;
      default:
        assert(false);
    }
  }
  return rc;
}


RC TPCCTxnManager::run_tpcc_phase2() {
  TPCCQuery* tpcc_query = (TPCCQuery*) query;
  RC rc = RCOK;
  assert(CC_ALG == CALVIN);

	uint64_t w_id = tpcc_query->w_id;
  uint64_t d_id = tpcc_query->d_id;
  uint64_t c_id = tpcc_query->c_id;
  //uint64_t d_w_id = tpcc_query->d_w_id;
  //uint64_t c_w_id = tpcc_query->c_w_id;
  //uint64_t c_d_id = tpcc_query->c_d_id;
	//char * c_last = tpcc_query->c_last;
  //double h_amount = tpcc_query->h_amount;
	//bool by_last_name = tpcc_query->by_last_name;
	bool remote = tpcc_query->remote;
	uint64_t ol_cnt = tpcc_query->ol_cnt;
	uint64_t o_entry_d = tpcc_query->o_entry_d;
  //uint64_t o_id = tpcc_query->o_id;

	uint64_t part_id_w = wh_to_part(w_id);
	//uint64_t part_id_c_w = wh_to_part(c_w_id);
  bool w_loc = GET_NODE_ID(part_id_w) == g_node_id;
  //bool c_w_loc = GET_NODE_ID(part_id_c_w) == g_node_id;


	switch (tpcc_query->txn_type) {
		case TPCC_PAYMENT :
      break;
		case TPCC_NEW_ORDER :
      if(w_loc) {
			  rc = new_order_0( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
        rc = new_order_1( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
        rc = new_order_2( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
        rc = new_order_3( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
        rc = new_order_4( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
        tpcc_query->o_id = *(int64_t *) row->get_value(D_NEXT_O_ID);
        //rc = new_order_5( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      }
        for(uint64_t i = 0; i < tpcc_query->ol_cnt; i++) {

          uint64_t ol_number = i;
          uint64_t ol_i_id = tpcc_query->items[ol_number]->ol_i_id;
          uint64_t ol_supply_w_id = tpcc_query->items[ol_number]->ol_supply_w_id;
          //uint64_t ol_quantity = tpcc_query->items[ol_number].ol_quantity;
          //uint64_t ol_amount = tpcc_query->ol_amount;
          uint64_t part_id_ol_supply_w = wh_to_part(ol_supply_w_id);
          bool ol_supply_w_loc = GET_NODE_ID(part_id_ol_supply_w) == g_node_id;
          if(ol_supply_w_loc) {
            rc = new_order_6(ol_i_id, row);
            rc = new_order_7(ol_i_id, row);
          }
        }
        break;
    default: assert(false);
  }
  return rc;
}

RC TPCCTxnManager::run_tpcc_phase5() {
  TPCCQuery* tpcc_query = (TPCCQuery*) query;
  RC rc = RCOK;
  assert(CC_ALG == CALVIN);

	uint64_t w_id = tpcc_query->w_id;
  uint64_t d_id = tpcc_query->d_id;
  uint64_t c_id = tpcc_query->c_id;
  uint64_t d_w_id = tpcc_query->d_w_id;
  uint64_t c_w_id = tpcc_query->c_w_id;
  uint64_t c_d_id = tpcc_query->c_d_id;
	char * c_last = tpcc_query->c_last;
  double h_amount = tpcc_query->h_amount;
	bool by_last_name = tpcc_query->by_last_name;
	bool remote = tpcc_query->remote;
	uint64_t ol_cnt = tpcc_query->ol_cnt;
	uint64_t o_entry_d = tpcc_query->o_entry_d;
  uint64_t o_id = tpcc_query->o_id;

	uint64_t part_id_w = wh_to_part(w_id);
	uint64_t part_id_c_w = wh_to_part(c_w_id);
  bool w_loc = GET_NODE_ID(part_id_w) == g_node_id;
  bool c_w_loc = GET_NODE_ID(part_id_c_w) == g_node_id;


	switch (tpcc_query->txn_type) {
		case TPCC_PAYMENT :
      if(w_loc) {
        rc = run_payment_0(w_id, d_id, d_w_id, h_amount, row);
        rc = run_payment_1(w_id, d_id, d_w_id, h_amount, row);
        rc = run_payment_2(w_id, d_id, d_w_id, h_amount, row);
        rc = run_payment_3(w_id, d_id, d_w_id, h_amount, row);
      }
      if(c_w_loc) {
        rc = run_payment_4( w_id,  d_id, c_id, c_w_id,  c_d_id, c_last, h_amount, by_last_name, row); 
        rc = run_payment_5( w_id,  d_id, c_id, c_w_id,  c_d_id, c_last, h_amount, by_last_name, row); 
      }
      break;
		case TPCC_NEW_ORDER :
      if(w_loc) {
        //rc = new_order_4( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
        rc = new_order_5( w_id, d_id, c_id, remote, ol_cnt, o_entry_d, &tpcc_query->o_id, row); 
      }
        for(uint64_t i = 0; i < tpcc_query->ol_cnt; i++) {

          uint64_t ol_number = i;
          uint64_t ol_i_id = tpcc_query->items[ol_number]->ol_i_id;
          uint64_t ol_supply_w_id = tpcc_query->items[ol_number]->ol_supply_w_id;
          uint64_t ol_quantity = tpcc_query->items[ol_number]->ol_quantity;
          uint64_t ol_amount = tpcc_query->ol_amount;
          uint64_t part_id_ol_supply_w = wh_to_part(ol_supply_w_id);
          bool ol_supply_w_loc = GET_NODE_ID(part_id_ol_supply_w) == g_node_id;
          if(ol_supply_w_loc) {
            rc = new_order_8( w_id, d_id, remote, ol_i_id, ol_supply_w_id, ol_quantity,  ol_number, o_id, row); 
            rc = new_order_9( w_id, d_id, remote, ol_i_id, ol_supply_w_id, ol_quantity,  ol_number, ol_amount, o_id, row); 
          }
        }
        break;
    default: assert(false);
  }
  return rc;

}

