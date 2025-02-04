#include <cinttypes>

#include "db/builder.h"
#include "db/db_impl/db_impl.h"
#include "db/error_handler.h"
#include "db/periodic_task_scheduler.h"
#include "env/composite_env_wrapper.h"
#include "file/filename.h"
#include "file/read_write_util.h"
#include "file/sst_file_manager_impl.h"
#include "file/writable_file_writer.h"
#include "logging/logging.h"
#include "monitoring/persistent_stats_history.h"
#include "monitoring/thread_status_util.h"
#include "options/options_helper.h"
#include "rocksdb/table.h"
#include "rocksdb/wal_filter.h"
#include "test_util/sync_point.h"
#include "util/rate_limiter_impl.h"
#include "util/udt_util.h"

#include "rocksdb/db_master.h"
#include "rocksdb/db.h"
#include "rocksdb/status.h"
#include "rocksdb/clue_entry_set.h"
#include <stdlib.h>
#include <string>
#include <sched.h>

#include <chrono>
#include <fstream>
#include <filesystem> 
#include <stdexcept> 
#include <vector>


struct OperationTime {
    std::chrono::high_resolution_clock::time_point start;
    std::chrono::high_resolution_clock::time_point mid;
    std::chrono::high_resolution_clock::time_point end;
};

std::vector<OperationTime> times_put;
std::vector<OperationTime> times_hash;
std::vector<OperationTime> times_fb_ce;
std::vector<OperationTime> times_fb_tkv;

std::vector<OperationTime> times_get;
std::vector<OperationTime> times_get_ce;

//std::vector<std::pair<int, int>> times_RD;
//std::vector<int> times_stall;

struct stalllog{
  short int type;
  int time;
};
int times_stall[2][100000000];

struct stalllog stall_log[100000];

int stall_idx = 0;

std::mutex mtx;

int count_put = 0;

unsigned long long int ce_count = 0;
unsigned long long int ce_get_count = 0;
unsigned long long int ce_get_success_count = 0;

unsigned long long int stall_count_tkv = 0;
unsigned long long int stall_count_pri = 0;
unsigned long long int stall_count_ce = 0;
unsigned long long int stall_count_fb = 0;

unsigned long long int flush_pending_tkv = 0;
unsigned long long int flush_pending_pri = 0;
unsigned long long int flush_pending_ce = 0;
unsigned long long int flush_pending_fb = 0;

unsigned long long int delay_count_tkv = 0;
unsigned long long int delay_count_pri = 0;
unsigned long long int delay_count_ce = 0;
unsigned long long int delay_count_fb = 0;

unsigned long long int delay_pend_tkv = 0;
unsigned long long int delay_pend_pri = 0;
unsigned long long int delay_pend_ce = 0;
unsigned long long int delay_pend_fb = 0;

int monitor_interval;

#define __int64 long long

std::atomic<bool> shouldExit(false); 
std::atomic<bool> rollback_should_exit(false); 
std::atomic<bool> finalexit(false); 
int L0_high;

bool iterator_seek[20];


namespace ROCKSDB_NAMESPACE{
  namespace fs = std::filesystem; // 네임스페이스 별칭 생성

Status DB_MASTER::Open(const Options& options, const std::string& name, const int num_instances_, const int monitor_interval_){

  monitor_interval = monitor_interval_;

  if(num_instances_ > MAX_DB_INSTANCE){
    return Status::NotSupported(
      "Open() max 100 instances.");
  }


  num_instances = num_instances_;

  cnt_primary_put = 0;
  cnt_primary_but_over_CTT = 0;
  cnt_primary_get = 0;

  /* open N instances */
  std::string *name_arr = new std::string[num_instances];

  for(int i = 0; i < num_instances; i++){
      name_arr[i] = name + std::to_string(i);
  }

  // init clue entry set
  ce_set_arr = new Clue_Entry_Set*[num_instances];
  for(int i = 0; i < num_instances; i++){
    ce_set_arr[i] = new Clue_Entry_Set();
  }


  rollback_mutex_arr = new std::mutex*[num_instances];
  rollback_write_mutex_arr = new std::mutex*[num_instances];
  for(int i = 0; i < num_instances; i++){
    rollback_mutex_arr[i] = new std::mutex();
    rollback_write_mutex_arr[i] = new std::mutex();
  }

  Status ret_val = Status::OK();

  //level0_file_num_compaction_trigger

  for(int i = 0; i < num_instances; i++){
    printf("Open DB path : %s\n", name_arr[i].c_str());
    Options tmp_option = Options(options);

    tmp_option.ce_set = ce_set_arr[i];
    tmp_option.db_master_ptr = this;
    tmp_option.rollback_mutex = rollback_mutex_arr[i];

    ////tmp_option.level0_file_num_compaction_trigger = compaction_trigger[i];
    
    ret_val = DB::Open(tmp_option, name_arr[i], &dbptr_array[i]);
    if(dbptr_array[i]->GetDBOptions().ce_set == nullptr){
      printf("[DEBUG] in open ce set null\n");
    }
    printf("[DEBUG] size in open%d\n",dbptr_array[i]->GetDBOptions().ce_set->size());
    if(!ret_val.ok()){
      printf("failed while opening DB path : %s\n", name_arr[i].c_str());
      break;
    }
  }
  
  //dbptr_array[0]->GetDBOptions(options);
  l0_stop_trigger = options.level0_stop_writes_trigger;
  l0_compaction_trigger = options.level0_file_num_compaction_trigger;
  l0_size = (int *)malloc(sizeof(int)*num_instances);
  for(int i=0; i<num_instances; i++){
    l0_size[i] = 0;
  }
  L0_high = 7;
  //L0_high = (int)l0_stop_trigger * 0.8;
  //local_l0_stall = (l0_stop_trigger / num_instances) - 1;
    
	l0_compaction_db_num = 0;

  set_memtalbe_size = options.write_buffer_size;
  total_memtable_size = set_memtalbe_size * options.max_write_buffer_number;

  /* create BG thread for monitoring L0 files */
  shouldExit = false;

  monitor_thread = std::thread(&rocksdb::DB_MASTER::Monitor_Consumer, this);
  monitor_thread.detach();

  monitor_L0_status_arr = new int[num_instances];
  memset(monitor_L0_status_arr, 0, sizeof(int) * num_instances);
  
  /* make sLSM score */
  for(int i=0; i<num_instances; i++){
    monitor_L0_status_arr[i] = LEVEL_LOW;
  }

  /* Iterator identify */
  for(int i=0; i<num_instances; i++){
    iterator_seek[i] = false;
  }

  //diable rollback threads
  rollback_should_exit = false;
  rollback_thread_arr = new std::thread[num_instances];
  for(int i = 0; i < num_instances; i++){
    rollback_thread_arr[i] = std::thread(&rocksdb::DB_MASTER::Rollback_Consumer, this, i);
    rollback_thread_arr[i].detach();
  }
  

  soft_pending_compaction_bytes_limit = options.soft_pending_compaction_bytes_limit;
  hard_pending_compaction_bytes_limit = options.hard_pending_compaction_bytes_limit;

  // Restore CE Set
  for(int i=0; i<num_instances; i++){
    std::string ckp_name = name_arr[i] + "/CESET.ckp";
    std::cout << "[DEBUG] " << ckp_name << std::endl;
    if (!fs::exists(ckp_name)) {
      std::cerr << "No file found with the name: " << ckp_name << std::endl;
      continue;
    }
    else{
      std::cout << "[DEBUG] " << ckp_name << " Reading " << std::endl;
      std::ifstream inFile(ckp_name, std::ios::binary);
      if (!inFile) {
        std::cerr << "Cannot open the file: " << ckp_name << std::endl;
        continue;
      }
      else{
        std::cout << "[DEBUG] " << ckp_name << " Map Reading " << std::endl;
        // 맵의 크기 읽기
        size_t ckp_size;
        inFile.read(reinterpret_cast<char*>(&ckp_size), sizeof(ckp_size));

        // 맵 데이터 읽기
        //std::unordered_map<std::string, std::string> ce_map;
        std::cout << "[DEBUG] " << ckp_name << " MapData Reading " << std::endl;
        for (size_t j = 0; j < ckp_size; ++j) {

          size_t keyLength;
          if (inFile.read(reinterpret_cast<char*>(&keyLength), sizeof(keyLength))) {
              std::string key(keyLength, '\0');
              if (inFile.read(&key[0], keyLength)) {
                  size_t valueLength;
                  if (inFile.read(reinterpret_cast<char*>(&valueLength), sizeof(valueLength))) {
                      std::string value(valueLength, '\0');
                      if (inFile.read(&value[0], valueLength)) {
                          ce_set_arr[i]->put(key, value);
                      } else {
                          std::cerr << "Error reading value for key: " << key << std::endl;
                      }
                  } else {
                      std::cerr << "Error reading value length for key: " << key << std::endl;
                  }
              } else {
                  std::cerr << "Error reading key." << std::endl;
              }
          } else {
              std::cerr << "Error reading key length." << std::endl;
          }

        }
        inFile.close();
      }
    }
  }


  return ret_val;
}


Status DB_MASTER::Put(const WriteOptions& options, const Slice& key, const Slice& value){

  int home_lsm_idx = hash_key(key);
  int target_lsm_idx = home_lsm_idx;

  std::string l0_num_str, memtable_size_str, pending_size_str;
  [[maybe_unused]]int l0_num_tmp;
  [[maybe_unused]]uint64_t pending_compaction_bytes;

  [[maybe_unused]]uint64_t memtable_size;
  Status ret_val;

  std::string write_stall_str, write_stall_str_fb, write_stall_str_ce;
  [[maybe_unused]] std::string str_tmp;
  [[maybe_unused]]int write_stall;

  // Make True-Entry (TE) 
  std::string value_str = std::string("a") + std::string(value.ToString());

  std::string flush_pen_str, flush_pen_str_fb, flush_pen_str_ce;
  
  std::string delay_count_str_tkv, delay_count_str_pri, delay_count_str_ce, delay_count_str_fb;
  //imt_num;
  OperationTime stall, stall_ce, stall_ce_for, stall_fb;

  
  if(monitor_L0_status_arr[home_lsm_idx] != LEVEL_HIGH){
    ret_val = dbptr_array[home_lsm_idx]->DB::Put(options, key, value_str);

  
    return ret_val;
  }


  for(int i=0; ; i++){
    if(i >= num_instances) i=0;
    mtx.lock();
    target_lsm_idx = (*slsms_ptr)[i].id;
    mtx.unlock();

    dbptr_array[target_lsm_idx]->GetProperty("rocksdb.is-write-stopped", &write_stall_str);    

    if(monitor_L0_status_arr[target_lsm_idx] != LEVEL_HIGH && stoi(write_stall_str) != 1){
    //if(monitor_L0_status_arr[target_lsm_idx] != LEVEL_HIGH){
      if(target_lsm_idx != home_lsm_idx){
        
        //stall_ce_for.end = std::chrono::high_resolution_clock::now();

        ret_val = dbptr_array[target_lsm_idx]->DB::Put(options, key, value_str);
        ret_val = dbptr_array[home_lsm_idx]->DB::Put(options, key, std::to_string(target_lsm_idx));

        ce_count++;
        //goto rtv;
        return ret_val;

      }
      else{
        stall_ce_for.end = std::chrono::high_resolution_clock::now();
        ret_val = dbptr_array[home_lsm_idx]->DB::Put(options, key, value_str);
    
        //goto rtv;
        return ret_val;
      }
    }
  }

}


Status DB_MASTER::Get(const ReadOptions& options, const Slice& key, std::string* value){

  OperationTime opTime_get, opTime_get_ce;

  Status ret_val = Status::NotFound("Nothing found.");

  std::string tmp_value;
  int target_lsm_idx;

  opTime_get.start = std::chrono::high_resolution_clock::now();
  opTime_get_ce.start = std::chrono::high_resolution_clock::now();

  if(dbptr_array[hash_key(key)]->Get(options, key, &tmp_value).Status::ok()){
    /* node found, ptr node or data node */
    if(tmp_value[0] == 'a'){ /* data node */
    
      ret_val = Status::OK();
      value->assign(tmp_value.substr(1, tmp_value.length()));

      opTime_get.end = std::chrono::high_resolution_clock::now();
      std::lock_guard<std::mutex> lock(mtx);
      times_get.push_back(opTime_get);
    }
    else{ /* ptr node */
      ce_get_count++;
      if(tmp_value.size() > 2){
        std::cout << "[DEBUG Get] value " << tmp_value << std::endl;
      }
      else{
        target_lsm_idx = std::stoi(tmp_value);
        opTime_get_ce.mid = std::chrono::high_resolution_clock::now();

        std::string tkv_value;
        if(dbptr_array[target_lsm_idx]->Get(options, key, &tkv_value).Status::ok()){
          ret_val = Status::OK();
          value->assign(tkv_value.substr(1, tkv_value.length()));

          ce_get_success_count++;
        }
        opTime_get_ce.end = std::chrono::high_resolution_clock::now();
        std::lock_guard<std::mutex> lock(mtx);
        times_get_ce.push_back(opTime_get_ce);
      }
    }
  }

  return ret_val;
}

Status DB_MASTER::GetPin(const ReadOptions& options, const Slice& key, PinnableSlice* pinnable_val){
  OperationTime opTime_get, opTime_get_ce;

  Status ret_val = Status::NotFound("Nothing found.");

  std::string tmp_value;
  int target_lsm_idx;


  int hashkey = hash_key(key);

  opTime_get.start = std::chrono::high_resolution_clock::now();
  opTime_get_ce.start = std::chrono::high_resolution_clock::now();

  if(dbptr_array[hashkey]->Get(options, 
          dbptr_array[hashkey]->DefaultColumnFamily(), key, pinnable_val).Status::ok()){
    /* node found, ptr node or data node */

    if(pinnable_val->ToString()[0] == 'a'){  /* data node */
      ret_val = Status::OK();

      opTime_get.end = std::chrono::high_resolution_clock::now();
      std::lock_guard<std::mutex> lock(mtx);
      times_get.push_back(opTime_get);


    }
    else{ /* ptr node */
      if(pinnable_val->size() > 2){
        std::cout << "[DEBUG Get] value " << tmp_value << std::endl;
      }
      else{
        target_lsm_idx = std::stoi(pinnable_val->ToString());
        opTime_get_ce.mid = std::chrono::high_resolution_clock::now();
        if(dbptr_array[target_lsm_idx]->Get(options, dbptr_array[target_lsm_idx]->DefaultColumnFamily(), key, pinnable_val).Status::ok()){
          ret_val = Status::OK();

        }
        opTime_get_ce.end = std::chrono::high_resolution_clock::now();
        std::lock_guard<std::mutex> lock2(mtx);
        times_get_ce.push_back(opTime_get_ce);

      }
    }
  
  }

  return ret_val;
}


void DB_MASTER::IteratorGet(Iterator** iter_to_use_master, std::unique_ptr<Iterator> *single_iter_master){

for(int i=0; i<num_instances; i++){
  iter_to_use_master[i] = single_iter_master[i].get();
 }

}

void DB_MASTER::IteratorReset(std::unique_ptr<Iterator> *single_iter_master, ReadOptions options){
  
  for(int i=0; i<num_instances; i++){
    single_iter_master[i].reset(dbptr_array[i]->NewIterator(options));
  }

}



void DB_MASTER::IteratorSeek(std::unique_ptr<Iterator> *single_iter_master, ReadOptions options,
                              Iterator** iter_to_use_master, const Slice& key){


  int idx = hash_key(key);
  //if(iterator_seek[idx] == false){
    single_iter_master[idx].reset(dbptr_array[idx]->NewIterator(options));
    iter_to_use_master[idx] = single_iter_master[idx].get();
    iterator_seek[idx] = true;
  //}

  iter_to_use_master[idx]->Seek(key);
  
  min_iterator_index = idx;


}

void DB_MASTER::ResetFlag(){
  for(int i=0; i<num_instances; i++){
    iterator_seek[i] = false;
  }
}

bool DB_MASTER::IteratorValid(Iterator** iter_to_use_master){

 bool rtv = false;
 if(iter_to_use_master[min_iterator_index]->Valid() == true){
  rtv = true;
 }
  return rtv;
}

bool DB_MASTER::IteratorCompare(Iterator** iter_to_use_master, const Slice& key, int* idx){
  
  bool rtv = false;

  
  int home_idx = hash_key(key);
  if(iter_to_use_master[home_idx]->Valid() && iter_to_use_master[home_idx]->key().compare(key) == 0){
    rtv = true;
    *idx = home_idx;
  }

  return rtv;

}


bool DB_MASTER::IteratorNext(std::unique_ptr<Iterator> *single_iter_master, ReadOptions options,
                            Iterator** iter_to_use_master){

  int next_idx_tmp = min_iterator_index+1;
  [[maybe_unused]] int next_idx;
 
  for(int i=0; i<num_instances; i++, next_idx_tmp++){
    if(next_idx_tmp >= num_instances){
      next_idx_tmp = 0;
    }
    if(iterator_seek[next_idx_tmp] == true){
 
      break;
    }
    else{       
      continue;
      return false;
      const Slice& key_tmp = iter_to_use_master[min_iterator_index]->key();

      single_iter_master[next_idx_tmp].reset(dbptr_array[next_idx_tmp]->NewIterator(options));
      iter_to_use_master[next_idx_tmp] = single_iter_master[next_idx_tmp].get();
      iter_to_use_master[next_idx_tmp]->Seek(key_tmp);
      iterator_seek[next_idx_tmp] = true;

  
      break;
      
    }
  }
  
  
  if(iter_to_use_master[min_iterator_index]->Valid() == false){
    return false;
    if(iter_to_use_master[next_idx_tmp]->Valid() == true){
      min_iterator_index = next_idx_tmp;
      return true;
    }
    else{
      return false;
    }
  }
  else{
    iter_to_use_master[min_iterator_index]->Next(); 
    if(iter_to_use_master[next_idx_tmp]->Valid() == true){
      int comp = iter_to_use_master[min_iterator_index]->key().compare(iter_to_use_master[next_idx_tmp]->key());
      if(comp > 0){
        min_iterator_index = next_idx_tmp;
      }
      return true;
    }
    else{
      return true;
    }
  }
  return false;
}

void DB_MASTER::IteratorPrev(Iterator** iter_to_use_master){

  for(int i=0; i<num_instances; i++){
    if(iter_to_use_master[i]->Valid()){
      iter_to_use_master[i]->Prev();
    }
  } 

}

Slice DB_MASTER::IteratorValue(Iterator** iter_to_use_master, int idx){
  return iter_to_use_master[idx]->value();
}

Slice DB_MASTER::IteratorKey(Iterator** iter_to_use_master, int idx){
  return iter_to_use_master[idx]->key();
}



Status DB_MASTER::DestroyDB_Master(const Options& options){
  Status ret_val = Status::OK();

  shouldExit = true;
  rollback_should_exit = true;
  
  for(int i = 0; i < num_instances; i++){
    std::string tmp = dbptr_array[i]->GetName();

    delete dbptr_array[i];

    dbptr_array[i] = nullptr;

    Status st = DestroyDB(tmp, options);
    if(!st.ok()){
      ret_val = st;
      printf("[DEBUG] DestoryDB_Master, %d-th not ok.\n", i);
    }
  }



  return ret_val;
}

int DB_MASTER::hash_key(const Slice& key){

  unsigned long long hex_int2 = (unsigned long long)strtoull(key.ToString(true).substr(0, 16).c_str(), NULL, 16);
  int hash = hex_int2 % num_instances;
  

  return hash;

}

void DB_MASTER::Monitor_Consumer(){
  std::string l0_num_str, memtable_size_str, pending_size_str, slsm_size_str;
  int l0_num_tmp;
  uint64_t pending_compaction_bytes;
  [[maybe_unused]]uint64_t memtable_size;
  [[maybe_unused]]uint64_t slsm_size;
  Level level_tmp;
  [[maybe_unused]]char level_c;
  [[maybe_unused]]std::string write_stall_str;
  [[maybe_unused]]int write_stall;
  [[maybe_unused]]std::string imt_str, mt_num_str, imt_num_str, cur_mem_str, mt_pend_str;
  [[maybe_unused]]int mt_num, imt_num;
  [[maybe_unused]]uint64_t imt, cur_mem;
  //std::string rocksdb_stats;

  std::vector<sLSM>* slsms_tmp = nullptr;
  std::vector<sLSM>* slsms_ready = nullptr;
  slsms_tmp = new std::vector<sLSM>;
  int interval_count = 0;

  usleep(1000000);
  while(!shouldExit.load()){
    interval_count += monitor_interval;
    //check every interval : hyperparameter
    usleep(monitor_interval);
    slsms_tmp->clear();


    for(int idx = 0; idx < num_instances; idx++){

      dbptr_array[idx]->GetProperty("rocksdb.num-files-at-level0", &l0_num_str);
      l0_num_tmp = stoi(l0_num_str);

      // for MMO 
      dbptr_array[idx]->GetProperty("rocksdb.size-all-mem-tables", &memtable_size_str);
      memtable_size = stoull(memtable_size_str);

      // for RDO 
      dbptr_array[idx]->GetProperty("rocksdb.estimate-pending-compaction-bytes", &pending_size_str);
      pending_compaction_bytes = stoull(pending_size_str);

      // for slsm size 
      dbptr_array[idx]->GetProperty("rocksdb.total-sst-files-size", &slsm_size_str);
      slsm_size = stoull(slsm_size_str); 


      if(l0_num_tmp >= l0_stop_trigger - 1 ||
          l0_num_tmp + (int)(memtable_size / set_memtalbe_size) >= l0_stop_trigger - 1 ||
        pending_compaction_bytes >= (hard_pending_compaction_bytes_limit - RESERVE)){

        level_tmp = LEVEL_HIGH;
      }
      else if(l0_num_tmp >= L0_high ||
      pending_compaction_bytes >= (soft_pending_compaction_bytes_limit + hard_pending_compaction_bytes_limit) / 2){
        level_tmp = LEVEL_MIDDLE;
      }
      else{
        level_tmp = LEVEL_LOW;
      }

      if(l0_num_tmp == 0){
        level_tmp = LEVEL_ZERO;
      }

      monitor_L0_status_arr[idx] = level_tmp;
      slsms_tmp->push_back({idx, l0_num_tmp, slsm_size});
    }
    
    /* sorting score */
    std::sort(slsms_tmp->begin(), slsms_tmp->end());

      mtx.lock();
      {
        slsms_ready = slsms_ptr;
        slsms_ptr = slsms_tmp;
        delete slsms_ready;
        
        slsms_tmp = new std::vector<sLSM>;
      }
      mtx.unlock();
    
    if(interval_count >= 1000000){ //every 1s
       int totalL0tmp = 0;
        printf("[DEBUG] Monitor LSM Level0|");

        for(int i = 0; i < num_instances; i++){
          dbptr_array[i]->GetProperty("rocksdb.num-files-at-level0", &l0_num_str);
          l0_num_tmp = stoi(l0_num_str);
          printf("%2d|", l0_num_tmp);
          totalL0tmp += l0_num_tmp;
        }
        totalL0Num = totalL0tmp;
        printf("%d \n", totalL0Num);

       printf("[DEBUG] Monitor LSM Status|");
        for(int i = 0; i < num_instances; i++){
          if(monitor_L0_status_arr[i] == LEVEL_LOW){
            level_c = 'L';
          }
          else if(monitor_L0_status_arr[i] == LEVEL_MIDDLE){
            level_c = 'M';
          }
          else if(monitor_L0_status_arr[i] == LEVEL_HIGH){
            level_c = 'H';
          }
          else if(monitor_L0_status_arr[i] == LEVEL_ZERO){
            level_c = 'O';
          }
          else{
            level_c = '?';
          }
          printf("%c|", level_c);
        }
        printf("\n");

        
        for(int i = 0; i < num_instances; i++){
          printf("[DEBUG] Monitor Fallback Priority : ID %d L0 %d Size %lu\n", (*slsms_ptr)[i].id, (*slsms_ptr)[i].l0, (*slsms_ptr)[i].size);
        }
        
        //ksm : to check ce set
        unsigned int total_ce = 0;
        printf("[DEBUG] Monitor CE set size|");
        for(int i = 0; i < num_instances; i++){
          printf(" %d|", ce_set_arr[i]->size());
          total_ce += (unsigned int) ce_set_arr[i]->size();
        }
        printf("%d\n", total_ce);
        totalCENum = total_ce;

        interval_count = 0;

    }
   
  }



  slsms_ptr->clear();
  free(monitor_L0_status_arr);

  printf("[DEBUG] Monitor module down\n");

}


void DB_MASTER::Rollback_Consumer(int lsm_idx){
  printf("[DEBUG] rollback_consumer_running%d|%d|\n", lsm_idx, ce_set_arr[lsm_idx]->size());

  std::string key, clue_value, true_value, tmp_value;
  [[maybe_unused]]int fallback_idx;
  usleep(5000000); //5sec
  PinnableSlice pinnable_val;


  while (!rollback_should_exit.load()){
    usleep(1000000);
    if(rollback_should_exit.load()) break;

    
    int zeroCount = 0;
    for(int i=0; i<num_instances; i++){
      if(monitor_L0_status_arr[i] <= LEVEL_LOW){
      //if(monitor_L0_status_arr[i] == LEVEL_ZERO){
        zeroCount++;
      }
    }
    
    //disableed
    if(false){
      ce_set_arr[lsm_idx]->rollback = true;

      while(ce_set_arr[lsm_idx]->size() > 0 ){

        ce_set_arr[lsm_idx]->getOnePair(&key, &clue_value);
        if(clue_value.length() < 1) {
          printf("[DEBUG CE] ce is empty %s\n", clue_value.c_str());
          continue;
        }


        try {
        fallback_idx = stoi(clue_value);
        } catch (const std::invalid_argument& e) {
            fallback_idx = -1; 
            continue;
        } catch (const std::out_of_range& e) {

            fallback_idx = -1;
            continue;
        }

        dbptr_array[fallback_idx]->Get(ReadOptions(), dbptr_array[fallback_idx]->DefaultColumnFamily(), key, &pinnable_val);

        dbptr_array[lsm_idx]->Put(WriteOptions(), key, pinnable_val.ToString());

        ce_set_arr[lsm_idx]->remove(key);

        pinnable_val.Reset();


      }


    }
  }
}

int DB_MASTER::totalL0(void){
  return totalL0Num;
}

unsigned int DB_MASTER::totalCE(void){
  return totalCENum;
}

void DB_MASTER::printstat(void){
  std::string stats;
  for(int i=0; i<num_instances; i++){
    dbptr_array[i]->GetProperty("rocksdb.stats", &stats);
    std::cout << "\n******************** LSM-Shard " << i << " *************************\n" << std::endl;

    std::cout << stats << std::endl;

    std::cout << "------rocksdb.options-statistics-------" << std::endl;
    dbptr_array[i]->GetProperty("rocksdb.options-statistics", &stats);
    std::cout << stats << std::endl;

    //rocksdb.db-write-stall-stats

    std::cout << "------rocksdb.db-write-stall-stats-------" << std::endl;
    dbptr_array[i]->GetProperty("rocksdb.db-write-stall-stats", &stats);
    std::cout << stats << std::endl;

  }




  usleep(5000000); //5sec
}

void DB_MASTER::PutThroughCE(const WriteOptions& options, const Slice& key, const Slice& value){
  int primary_idx = hash_key(key);

  int fallback_idx;
  for(fallback_idx = 0; fallback_idx < num_instances; fallback_idx++){
    if(fallback_idx != primary_idx){
      break;
    }
  }

  ce_set_arr[primary_idx]->put(key.ToString(true), std::to_string(fallback_idx));

  dbptr_array[fallback_idx]->Put(options, key, Slice("a" + value.ToString()));
}

}