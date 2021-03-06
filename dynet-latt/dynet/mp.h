
#pragma once
#if !_WINDOWS
#include "dynet/globals.h"
#include "dynet/dynet.h"
#include "dynet/training.h"
#include "dynet/expr.h"
#include "dynet/dict.h"
#include "dynet/lstm.h"
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/interprocess/anonymous_shared_memory.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <iostream>
#include <limits>
#include <fstream>
#include <vector>
#include <utility>
#include <sstream>
#include <random>
#include <algorithm>

#include <time.h>
#include <sys/time.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif


namespace dynet {
  namespace mp {

    // TODO: Pass these around instead of having them be global
    extern std::string queue_name;
    extern std::string shared_memory_name;
    extern timespec start_time, start_time_last;
    extern bool stop_requested;

    void GetUTCTime(timespec & ts);
    double ElapsedTime();
    double ElapsedTimeDelta();


    struct WorkloadHeader {
      bool is_dev_set;
      bool end_of_epoch;
      unsigned report_frequency;
      unsigned iter;
      real learning_scale;
    };

    // A simple struct to hold information about a child process
    // TODO: Rename me!
    struct Workload {
      pid_t pid;
      int c2p[2]; // Child to parent pipe
      int p2c[2]; // Parent to child pipe
    };

    // This interface is used by the child processes and called
    // once per datum.
    template<class D, class S>
    class ILearner {
    public:
      virtual ~ILearner() {}
      virtual void StartTrain() {};
      virtual void StartDev() {};
      virtual S LearnFromDatum(const D& datum, bool learn) = 0;
      virtual void SaveModelDevBest() {};
      virtual void SaveModelTrainBest() {};
      virtual int GetNumSentForDatum(D datum) {return 1;}
      virtual int GetNumWordsForDatum(D datum) {return 1;}
      virtual void ReportStep(unsigned cid, WorkloadHeader& header, unsigned num_sent, unsigned sent_delta,
			  unsigned num_words, unsigned words_delta, double time, double time_delta,
			  S loss_delta){
	if (cid == 0) {
	  std::cerr << (header.is_dev_set ? "dev" : "train") << " sent " << num_sent << " loss: " << loss_delta << std::endl;
	}
      }
      virtual void ReportEvalPoint(bool dev, double fractional_iter, S total_loss, bool new_best) {
	if(dev) std::cerr << fractional_iter << "\t" << "dev loss = " << total_loss << (new_best ? " (New best!)" : "") << std::endl;
	else std::cerr << fractional_iter << "\t" << "loss = " << total_loss << std::endl;
      }

    };

    struct SharedObject {
      SharedObject() : update_mutex(1), counter_mutex(1), counter(0) {} 
      boost::interprocess::interprocess_semaphore update_mutex;
      boost::interprocess::interprocess_semaphore counter_mutex;
      unsigned counter;
    };
    extern SharedObject* shared_object;

    /// XXX: We never delete these objects
    template <class T>
    T* get_shared_memory() {
      /*std::cerr << "Creating shared memory named " << shared_memory_name << std::endl;
      auto shm = new boost::interprocess::shared_memory_object(boost::interprocess::create_only, shared_memory_name.c_str(), boost::interprocess::read_write);
      shm->truncate(sizeof(T));
      auto region = new boost::interprocess::mapped_region (*shm, boost::interprocess::read_write);*/
      auto region = new boost::interprocess::mapped_region(boost::interprocess::anonymous_shared_memory(sizeof(T)));
      void* addr = region->get_address();
      T* obj = new (addr) SharedObject();
      return obj;
    }

    // Some simple functions that do IO to/from pipes.
    // These are used to send data from child processes
    // to the parent process or vice/versa.
    template <class T>
    T read_data(int pipe) {
      T v;
      int err = read(pipe, (void*)&v, sizeof(T));
      assert (err != -1);
      return v;
    }

    template <class T>
    void write_data(int pipe, const T& v) {
      int err = write(pipe, (void*)&v, sizeof(T));
      assert (err != -1);
    }

    std::string generate_queue_name();
    std::string generate_shared_memory_name();

    dynet::real sum_values(const std::vector<dynet::real>& values);
    dynet::real mean(const std::vector<dynet::real>& values);

    std::string elapsed_time_string(const timespec& start, const timespec& end);

    unsigned spawn_children(std::vector<Workload>& workloads);
    std::vector<Workload> create_workloads(unsigned num_children);

    // Called by the parent to process a chunk of data
    template <class S>
    S run_data_set(std::vector<unsigned>::iterator begin, std::vector<unsigned>::iterator end, const std::vector<Workload>& workloads,
        boost::interprocess::message_queue& mq, const WorkloadHeader& header) {
      const unsigned num_children = workloads.size();

      // Tell all the children to start up
      for (unsigned cid = 0; cid < num_children; ++cid) {
        bool cont = true;
        write_data(workloads[cid].p2c[1], cont);
        write_data(workloads[cid].p2c[1], header);
      }

      // Write all the indices to the queue for the children to process
      for (auto curr = begin; curr != end; ++curr) {
        unsigned i = *curr;
        mq.send(&i, sizeof(i), 0);
        if (stop_requested) {
          break;
        }
      }

      // Send a bunch of stop messages to the children
      for (unsigned cid = 0; cid < num_children; ++cid) {
        unsigned stop = -1U;
        mq.send(&stop, sizeof(stop), (stop_requested ? 1 : 0));
      }

      // Wait for each child to finish training its load
      std::vector<S> losses(num_children);
      for(unsigned cid = 0; cid < num_children; ++cid) {
        losses[cid] = read_data<S>(workloads[cid].c2p[0]);
      }

      S total_loss = S();
      for (S& datum_loss : losses) {
        total_loss += datum_loss;
      }
      return total_loss;
    }

    // TODO: move this and other implementations to .cc file
    template<class D, class S>
    void run_parent(const std::vector<D>& train_data, const std::vector<D>& dev_data, ILearner<D, S>* learner,
       std::vector<Workload>& workloads, unsigned num_iterations, unsigned dev_frequency, unsigned report_frequency, real rate_decay, real scale_thresh) {
      const unsigned num_children = workloads.size();
      boost::interprocess::message_queue mq(boost::interprocess::open_or_create, queue_name.c_str(), 10000, sizeof(unsigned));
      std::vector<unsigned> train_indices(train_data.size());
      std::iota(train_indices.begin(), train_indices.end(), 0);

      std::vector<unsigned> dev_indices(dev_data.size());
      std::iota(dev_indices.begin(), dev_indices.end(), 0);

      S best_dev_loss = S();
      S best_train_loss = S();
      S last_loss = S();
      bool first_dev_run = true, first_train_run=true;
      bool do_dev = dev_data.size() > 0;
      real learning_scale = 1.0;
      for (unsigned iter = 0; iter < num_iterations && !stop_requested; ++iter) {
        // Shuffle the training data indices
        std::shuffle(train_indices.begin(), train_indices.end(), *rndeng);

        S train_loss = S();

        std::vector<unsigned>::iterator begin = train_indices.begin();
        while (begin != train_indices.end()) {
          std::vector<unsigned>::iterator end = begin + dev_frequency;
          if (end > train_indices.end()) {
            end = train_indices.end();
          }
          bool end_of_epoch = (end == train_indices.end());
//          double fractional_iter = iter + 1.0 * distance(train_indices.begin(), end) / train_indices.size();
          learner->StartTrain();
          S batch_loss = run_data_set<S>(begin, end, workloads, mq, {false, end_of_epoch, report_frequency, iter+1, learning_scale});
          train_loss += batch_loss;
          if(end_of_epoch) learner->ReportEvalPoint(false, iter+1, batch_loss, false);

          if (stop_requested) {
            break;
          }

          bool new_best_dev=false, new_best_train=false;
          S dev_loss;
          if(do_dev){
            learner->StartDev();
	    dev_loss = run_data_set<S>(dev_indices.begin(), dev_indices.end(), workloads, mq, {true, false, report_frequency, iter+1, learning_scale});
	    new_best_dev = (first_dev_run || dev_loss < best_dev_loss);
	    learner->ReportEvalPoint(true, iter+1, dev_loss, new_best_dev);
          }
          new_best_train = (first_train_run || train_loss < best_train_loss);

          if(do_dev){
            if(!first_dev_run && last_loss < dev_loss){
	      learning_scale *= rate_decay;
	      if(learning_scale < scale_thresh) stop_requested = true;
            }
          } else {
            if(!first_train_run && end_of_epoch && last_loss < train_loss){
  	      learning_scale *= rate_decay;
  	      if(learning_scale < scale_thresh) stop_requested = true;
            }
          }

          if (do_dev && new_best_dev) {
            learner->SaveModelDevBest();
            best_dev_loss = dev_loss;
          }
          if (new_best_train && end_of_epoch) {
            learner->SaveModelTrainBest();
            best_train_loss = train_loss;
          }

          if (stop_requested) {
	    break;
	  }

          last_loss = (do_dev?dev_loss:train_loss);
	  first_dev_run = false;
	  if(end_of_epoch) first_train_run = false;

          begin = end;
        }
      }

      // Kill all children one by one and wait for them to exit
      for (unsigned cid = 0; cid < num_children; ++cid) {
        bool cont = false;
        write_data(workloads[cid].p2c[1], cont);
        wait(NULL);
      }
    }

    template <class D, class S>
    int run_child(unsigned cid, ILearner<D, S>* learner, Trainer* trainer,
        std::vector<Workload>& workloads, const std::vector<D>& train_data,
        const std::vector<D>& dev_data) {
      const unsigned num_children = workloads.size();
      assert (cid >= 0 && cid < num_children);
      unsigned i;
      unsigned priority;
      boost::interprocess::message_queue::size_type recvd_size;
      boost::interprocess::message_queue mq(boost::interprocess::open_or_create, queue_name.c_str(), 10000, sizeof(unsigned));
      while (true) {
        // Check if the parent wants us to exit
        bool cont = read_data<bool>(workloads[cid].p2c[0]);
        if (cont == 0) {
          break;
        }

        // Check if we're running on the training data or the dev data 
        WorkloadHeader header = read_data<WorkloadHeader>(workloads[cid].p2c[0]);

        // Run the actual training loop
        S total_loss = S();
        S batch_loss = S();
        unsigned sent_counter = 0, sent_counter_last = 0;
        unsigned word_counter = 0, word_counter_last = 0;
        unsigned last_report = 0;
        while (true) {
          mq.receive(&i, sizeof(unsigned), recvd_size, priority);
          if (i == -1U) {
            break;
          }

          assert (i < (header.is_dev_set ? dev_data.size() : train_data.size()));
          const D& datum = (header.is_dev_set ? dev_data[i] : train_data[i]);
          S datum_loss = learner->LearnFromDatum(datum, !header.is_dev_set);
          total_loss += datum_loss;
          batch_loss += datum_loss;
          sent_counter += learner->GetNumSentForDatum(datum);
          word_counter += learner->GetNumWordsForDatum(datum);

          bool do_update = !header.is_dev_set && cid == 0;
          unsigned counter = 0;
          if (!header.is_dev_set) {
            shared_object->counter_mutex.wait();
            counter = ++shared_object->counter;
            if (do_update) { shared_object->counter = 0; }
            shared_object->counter_mutex.post();
          }
          if (do_update) {
            shared_object->update_mutex.wait();
//           trainer->update(1.0 / counter * header.learning_scale);
            trainer->update(1.0 * header.learning_scale);
            shared_object->update_mutex.post();
          }
          if (sent_counter / header.report_frequency != last_report) {
	    learner->ReportStep(cid, header, sent_counter, sent_counter-sent_counter_last, word_counter, word_counter-word_counter_last,
			    ElapsedTime(), ElapsedTimeDelta(), batch_loss);
            last_report = sent_counter / header.report_frequency;
            word_counter_last = word_counter;
            sent_counter_last = sent_counter;
            batch_loss = S();
          }
        }
        if (header.end_of_epoch) {
          trainer->update_epoch(1.0 / num_children);
        }

        // Let the parent know that we're done and return the loss value
        write_data(workloads[cid].c2p[1], total_loss);

      }
      return 0;
    }

    template<class D, class S>
    void run_multi_process(unsigned num_children, ILearner<D, S>* learner, Trainer* trainer, const std::vector<D>& train_data,
        const std::vector<D>& dev_data, unsigned num_iterations, unsigned dev_frequency, unsigned report_frequency,
	real rate_decay=1.0, real rate_thresh=0.0) {
      //assert (dynet::ps->is_shared());
      queue_name = generate_queue_name();
      boost::interprocess::message_queue::remove(queue_name.c_str());
      boost::interprocess::message_queue::remove(queue_name.c_str());
      shared_memory_name = generate_shared_memory_name();
      shared_object = get_shared_memory<SharedObject>();
      std::vector<Workload> workloads = create_workloads(num_children);
      GetUTCTime(start_time);
      GetUTCTime(start_time_last);
      unsigned cid = spawn_children(workloads);
      if (cid < num_children) {
        run_child(cid, learner, trainer, workloads, train_data, dev_data);
      }
      else {
        run_parent(train_data, dev_data, learner, workloads, num_iterations, dev_frequency, report_frequency, rate_decay, rate_thresh / trainer->eta0);
      }
    }

    template<class D, class S>
    void run_single_process(ILearner<D, S>* learner, Trainer* trainer, const std::vector<D>& train_data,
        const std::vector<D>& dev_data, unsigned num_iterations, unsigned dev_frequency, unsigned report_frequency, unsigned batch_size,
	real rate_decay=1.0, real rate_thresh=0.0) {
      std::vector<unsigned> train_indices(train_data.size());
      std::iota(train_indices.begin(), train_indices.end(), 0);

      std::vector<unsigned> dev_indices(dev_data.size());
      std::iota(dev_indices.begin(), dev_indices.end(), 0);

      GetUTCTime(start_time);
      GetUTCTime(start_time_last);

      S best_dev_loss = S();
      S best_train_loss = S();
      S last_loss = S();
      bool first_dev_run = true;
      bool first_train_run = true;
      bool do_dev = dev_data.size() > 0;
      unsigned batch_counter = 0;
      real learning_scale = 1.0;
      for (unsigned iter = 0; iter < num_iterations && !stop_requested; ++iter) {
        // Shuffle the training data indices
        std::shuffle(train_indices.begin(), train_indices.end(), *rndeng);

        S train_loss = S();

        unsigned data_processed = 0;
        unsigned sent_counter = 0, word_counter = 0;
        unsigned sent_counter_last = 0, word_counter_last = 0;
        int data_until_report = report_frequency;
        std::vector<unsigned>::iterator begin = train_indices.begin();
        while (begin != train_indices.end()) {
          std::vector<unsigned>::iterator end = begin + dev_frequency;
          if (end > train_indices.end()) {
            end = train_indices.end();
          }
          bool end_of_epoch = (end == train_indices.end());
          S batch_loss;
          learner->StartTrain();
          for (auto it = begin; it != end; ++it) {
            unsigned i = *it;
            assert (i < train_data.size());
            const D& datum = train_data[i];
            sent_counter += learner->GetNumSentForDatum(datum);
            word_counter += learner->GetNumWordsForDatum(datum);
            S datum_loss = learner->LearnFromDatum(datum, true);
            batch_loss += datum_loss;
            train_loss += datum_loss;
            if (++batch_counter == batch_size) {
//              trainer->update(1.0 / batch_size * learning_scale);
              trainer->update(1.0 / learning_scale);
              batch_counter = 0;
            }
            data_processed++;

            data_until_report -= learner->GetNumSentForDatum(datum);
            if (data_until_report <= 0) {
              data_until_report = report_frequency;
              WorkloadHeader header = {false, false, report_frequency, iter+1, learning_scale};
              learner->ReportStep(0, header, sent_counter, sent_counter-sent_counter_last,
			      word_counter, word_counter-word_counter_last,
              		      ElapsedTime(), ElapsedTimeDelta(), batch_loss);
              sent_counter_last = sent_counter;
              word_counter_last = word_counter;
              batch_loss = S();
            }
          }

          if (stop_requested) {
            break;
          }

//          double fractional_iter = iter + 1.0 * data_processed / train_indices.size();
          bool new_best_dev = false, new_best_train;
	  S dev_loss;
          if(do_dev){
            learner->StartDev();
	    for (auto it = dev_indices.begin(); it != dev_indices.end(); ++it) {
	      unsigned i = *it;
	      assert (i < dev_data.size());
	      const D& datum = dev_data[i];
	      S datum_loss = learner->LearnFromDatum(datum, false);
	      dev_loss += datum_loss;
	    }
	    new_best_dev = (first_dev_run || dev_loss < best_dev_loss);
          }
	  new_best_train = (first_train_run || train_loss < best_train_loss);

          if(end_of_epoch) learner->ReportEvalPoint(false, iter+1, train_loss, new_best_train);
          if(do_dev) learner->ReportEvalPoint(true, iter+1, dev_loss, new_best_dev);

          if(do_dev){
            if(!first_dev_run && last_loss < dev_loss){
	      learning_scale *= rate_decay;
	      if(learning_scale * trainer->eta0 < rate_thresh) stop_requested = true;
            }
          } else {
            if(end_of_epoch && !first_train_run && last_loss < train_loss){
	      learning_scale *= rate_decay;
	      if(learning_scale * trainer->eta0 < rate_thresh) stop_requested = true;
            }
          }

	  if (do_dev && new_best_dev) {
            learner->SaveModelDevBest();
            best_dev_loss = dev_loss;
          }
          if (new_best_train && end_of_epoch) {
            learner->SaveModelTrainBest();
            best_train_loss = train_loss;
          }

          if (stop_requested) {
            break;
          }

          trainer->update_epoch();

          last_loss = (do_dev?dev_loss:train_loss);
          first_dev_run = false;
          if(end_of_epoch) first_train_run = false;


          begin = end;
        }
      }
    }
  }
}
#endif // !_WINDOWS
