#include "td/utils/port/signals.h"
#include "td/utils/OptionParser.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/check.h"

#include "crypto/vm/cp0.h"

#include "DbScanner.h"
#include "EventProcessor.h"
#include "TraceInserter.h"


int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  CHECK(vm::init_op_cp0());

  // options
  std::string db_root;
  td::uint32 threads = 7;
  
  
  td::OptionParser p;
  p.set_description("Emulate TON traces");
  p.add_option('\0', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('D', "db", "Path to TON DB folder", [&](td::Slice fname) { 
    db_root = fname.str();
  });

  p.add_checked_option('t', "threads", "Scheduler threads (default: 7)", [&](td::Slice fname) { 
    int v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
    }
    threads = v;
    return td::Status::OK();
  });

  p.add_option('\0', "--redis", "Redis URI (default: 'tcp://127.0.0.1:6379')", [&](td::Slice fname) { 
    TraceInserter::redis_uri = fname.str();
  });


  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "failed to parse options: " << S.move_as_error();
    std::_Exit(2);
  }

  if (db_root.size() == 0) {
    std::cerr << "'--db' option missing" << std::endl;
    std::_Exit(2);
  }

  td::actor::Scheduler scheduler({threads});
  td::actor::ActorOwn<DbScanner> db_scanner;

  scheduler.run_in_context([&] { 
    db_scanner = td::actor::create_actor<DbScanner>("scanner", db_root, dbs_readonly);
    td::actor::create_actor<TraceEmulatorScheduler>("integritychecker", 
                          db_scanner.get()).release();
  });
  
  scheduler.run();

  return 0;
}
