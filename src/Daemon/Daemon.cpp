// Copyright (c) 2012-2017, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "DaemonCommandsHandler.h"

#include "Common/ScopeExit.h"
#include "Common/SignalHandler.h"
#include "Common/StdOutputStream.h"
#include "Common/StdInputStream.h"
#include "Common/PathTools.h"
#include "Common/Util.h"
#include "crypto/hash.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/DatabaseBlockchainCacheFactory.h"
#include "CryptoNoteCore/MainChainStorage.h"
#include "CryptoNoteCore/MinerConfig.h"
#include "CryptoNoteCore/RocksDBWrapper.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "P2p/NetNode.h"
#include "P2p/NetNodeConfig.h"
#include "Rpc/RpcServer.h"
#include "Rpc/RpcServerConfig.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"
#include "version.h"
#include <sqlite3.h>
#include <numeric>
#include <tuple>
#include <sstream>
#include <unordered_map>


#include <Logging/LoggerManager.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

#include <boost/tuple/tuple.hpp>
#include <boost/unordered_map.hpp>

typedef boost::tuples::tuple<uint64_t, uint64_t> Edge;

struct ihash
    : std::unary_function<Edge, std::size_t>
{
    std::size_t operator()(Edge const& e) const
    {
        std::size_t seed = 0;
        boost::hash_combine( seed, e.get<0>() );
        boost::hash_combine( seed, e.get<1>() );
        return seed;
    }
};

struct iequal_to
    : std::binary_function<Edge, Edge, bool>
{
    bool operator()(Edge const& x, Edge const& y) const
    {
        return ( x.get<0>()==y.get<0>() &&
                 x.get<1>()==y.get<1>());
    }
};

typedef boost::unordered_map< Edge, Crypto::Hash, ihash, iequal_to > EdgeMap;

using Common::JsonValue;
using namespace CryptoNote;
using namespace Logging;

namespace po = boost::program_options;

namespace
{
  const command_line::arg_descriptor<std::string> arg_config_file = {"config-file", "Specify configuration file", std::string(CryptoNote::CRYPTONOTE_NAME) + ".conf"};
  const command_line::arg_descriptor<bool>        arg_os_version  = {"os-version", ""};
  const command_line::arg_descriptor<std::string> arg_log_file    = {"log-file", "", ""};
  const command_line::arg_descriptor<int>         arg_log_level   = {"log-level", "", 2}; // info level
  const command_line::arg_descriptor<bool>        arg_console     = {"no-console", "Disable daemon console commands"};
  const command_line::arg_descriptor<bool>        arg_testnet_on  = {"testnet", "Used to deploy test nets. Checkpoints and hardcoded seeds are ignored, "
    "network id is changed. Use it with --data-dir flag. The wallet must be launched with --testnet flag.", false};
}

sqlite3* dbs;
char* zErrMsg = 0;
int rc;
char* sql;

void setupTable(){
  std::string sql_second_0 = "PRAGMA synchronous = OFF; \
  PRAGMA journal_mode = MEMORY; \
  CREATE TABLE inputs (iid INTEGER, mixin INTEGER, value INTEGER, anonset INTEGER);";
  sql = const_cast<char*>(sql_second_0.c_str());

  /* Execute SQL statement */
  rc = sqlite3_exec(dbs, sql, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }
  else {
    fprintf(stderr, "Creation done successfully\n");
  }
  std::string sql_second_1 = "PRAGMA synchronous = OFF; \
  PRAGMA journal_mode = MEMORY; \
  CREATE TABLE outputs (oid STRING, amount INTEGER, gidx INTEGER);";
  sql = const_cast<char*>(sql_second_1.c_str());

  /* Execute SQL statement */
  rc = sqlite3_exec(dbs, sql, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }
  else {
    fprintf(stderr, "Creation done successfully\n");
  }
    std::string sql_second_2 = "PRAGMA synchronous = OFF; \
  PRAGMA journal_mode = MEMORY; \
  CREATE TABLE refs (iid INTEGER, oid STRING);";
  sql = const_cast<char*>(sql_second_2.c_str());

  /* Execute SQL statement */
  rc = sqlite3_exec(dbs, sql, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }
  else {
    fprintf(stderr, "Creation done successfully\n");
  }
}


bool command_line_preprocessor(const boost::program_options::variables_map& vm, LoggerRef& logger);

JsonValue buildLoggerConfiguration(Level level, const std::string& logfile) {
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue& cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue& fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  JsonValue& consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "%D %T %L ");

  return loggerConfiguration;
}

int main(int argc, char* argv[])
{

#ifdef WIN32
  _CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif

  LoggerManager logManager;
  LoggerRef logger(logManager, "daemon");

  try {
    po::options_description desc_cmd_only("Command line options");
    po::options_description desc_cmd_sett("Command line options and settings options");

    command_line::add_arg(desc_cmd_only, command_line::arg_help);
    command_line::add_arg(desc_cmd_only, command_line::arg_version);
    command_line::add_arg(desc_cmd_only, arg_os_version);
    // tools::get_default_data_dir() can't be called during static initialization
    command_line::add_arg(desc_cmd_only, command_line::arg_data_dir, Tools::getDefaultDataDirectory());
    command_line::add_arg(desc_cmd_only, arg_config_file);

    command_line::add_arg(desc_cmd_sett, arg_log_file);
    command_line::add_arg(desc_cmd_sett, arg_log_level);
    command_line::add_arg(desc_cmd_sett, arg_console);
    command_line::add_arg(desc_cmd_sett, arg_testnet_on);

    RpcServerConfig::initOptions(desc_cmd_sett);
    NetNodeConfig::initOptions(desc_cmd_sett);
    DataBaseConfig::initOptions(desc_cmd_sett);

    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd_only).add(desc_cmd_sett);

    po::variables_map vm;
    boost::filesystem::path data_dir_path;
    bool r = command_line::handle_error_helper(desc_options, [&]()
    {
      po::store(po::parse_command_line(argc, argv, desc_options), vm);

      if (command_line::get_arg(vm, command_line::arg_help))
      {
        std::cout << CryptoNote::CRYPTONOTE_NAME << " v" << PROJECT_VERSION_LONG << ENDL << ENDL;
        std::cout << desc_options << std::endl;
        return false;
      }

      std::string data_dir = command_line::get_arg(vm, command_line::arg_data_dir);
      std::string config = command_line::get_arg(vm, arg_config_file);

      data_dir_path = data_dir;
      boost::filesystem::path config_path(config);
      if (!config_path.has_parent_path()) {
        config_path = data_dir_path / config_path;
      }

      boost::system::error_code ec;
      if (boost::filesystem::exists(config_path, ec)) {
        po::store(po::parse_config_file<char>(config_path.string<std::string>().c_str(), desc_cmd_sett), vm);
      }
      po::notify(vm);
      return true;
    });

    if (!r)
      return 1;
  
    auto modulePath = Common::NativePathToGeneric(argv[0]);
    auto cfgLogFile = Common::NativePathToGeneric(command_line::get_arg(vm, arg_log_file));

    if (cfgLogFile.empty()) {
      cfgLogFile = Common::ReplaceExtenstion(modulePath, ".log");
    } else {
      if (!Common::HasParentPath(cfgLogFile)) {
        cfgLogFile = Common::CombinePath(Common::GetPathDirectory(modulePath), cfgLogFile);
      }
    }

    Level cfgLogLevel = static_cast<Level>(static_cast<int>(Logging::ERROR) + command_line::get_arg(vm, arg_log_level));

    // configure logging
    logManager.configure(buildLoggerConfiguration(cfgLogLevel, cfgLogFile));

    logger(INFO) << CryptoNote::CRYPTONOTE_NAME << " v" << PROJECT_VERSION_LONG;

    if (command_line_preprocessor(vm, logger)) {
      return 0;
    }

    logger(INFO) << "Module folder: " << argv[0];

    bool testnet_mode = command_line::get_arg(vm, arg_testnet_on);
    if (testnet_mode) {
      logger(INFO) << "Starting in testnet mode!";
    }

    //create objects and link them
    CryptoNote::CurrencyBuilder currencyBuilder(logManager);
    currencyBuilder.testnet(testnet_mode);
    CryptoNote::Currency currency = currencyBuilder.currency();

    CryptoNote::Checkpoints checkpoints(logManager);
    if (!testnet_mode) {
      for (const auto& cp : CryptoNote::CHECKPOINTS) {
        checkpoints.addCheckpoint(cp.index, cp.blockId);
      }
    }
    
    NetNodeConfig netNodeConfig;
    netNodeConfig.init(vm);
    netNodeConfig.setTestnet(testnet_mode);

    RpcServerConfig rpcConfig;
    rpcConfig.init(vm);

    DataBaseConfig dbConfig;
    dbConfig.init(vm);

    if (dbConfig.isConfigFolderDefaulted()) {
      if (!Tools::create_directories_if_necessary(dbConfig.getDataDir())) {
        throw std::runtime_error("Can't create directory: " + dbConfig.getDataDir());
      }
    } else {
      if (!Tools::directoryExists(dbConfig.getDataDir())) {
        throw std::runtime_error("Directory does not exist: " + dbConfig.getDataDir());
      }
    }

    RocksDBWrapper database(logManager);
    database.init(dbConfig);
    Tools::ScopeExit dbShutdownOnExit([&database] () { database.shutdown(); });

    System::Dispatcher dispatcher;
    logger(INFO) << "Initializing core...";
    CryptoNote::Core ccore(
      currency,
      logManager,
      std::move(checkpoints),
      dispatcher,
      std::unique_ptr<IBlockchainCacheFactory>(new DatabaseBlockchainCacheFactory(database, logger.getLogger())),
      createSwappedMainChainStorage(data_dir_path.string(), currency));

    ccore.load();
    logger(INFO) << "Core initialized OK";

    int block_start = 1; 
    int block_stop = ccore.getTopBlockIndex();
    //int block_start = 1234568;
    //int block_stop = 1000;
    logger(INFO) << "The top block index is: " << block_stop;
    std::unordered_map<uint64_t, uint64_t> anonset;

    rc = sqlite3_open("/home/yorozuya/test.db", &dbs);
    if(rc){
      fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(dbs));
      exit(0);
    }
    else{
      fprintf(stderr, "Opened database successfully\n");
    }

    setupTable();
    int counter = 0;
    sqlite3_exec(dbs, "BEGIN TRANSACTION", NULL, NULL, &zErrMsg);
    for (int block_height = block_start; block_height <= block_stop; block_height++){
      //sqlite3_exec(dbs, "BEGIN TRANSACTION", NULL, NULL, &zErrMsg);

      Crypto::Hash block_hash = ccore.getBlockHashByIndex(block_height);
      BlockDetails block_detail = ccore.getBlockDetails(block_hash);
      std::vector<TransactionDetails> stupe = block_detail.transactions;
      std::cout << "Block height: " << block_detail.index << '\n';

      for (auto tx_details = stupe.begin(); tx_details != stupe.end(); ++tx_details){

        /*  
            handle the inputs of the transaction over here, also get the transaction
            from which the mixin originated
        */

        //std::cout << "Txhash: " << tx_hash << '\n';
        std::vector<TransactionInputDetails> tx_inputs = tx_details->inputs;
        uint64_t totalinputamt = tx_details->totalInputsAmount;
        if (totalinputamt == 0){
          //std::cout << "COINBASE!!!!!" << '\n';
        }
        else{
          for (auto input_tracker = tx_inputs.begin(); input_tracker != tx_inputs.end(); ++input_tracker){
            KeyInputDetails keyin = boost::get<KeyInputDetails>(*input_tracker);
            KeyInput input = keyin.input;
            uint64_t amount = input.amount;

            std::vector<uint32_t> outputIndexes = input.outputIndexes;
            int mixin = outputIndexes.size();
            std::vector<uint32_t> global_outputIndexes(mixin);
            std::partial_sum(outputIndexes.begin(), outputIndexes.end(), global_outputIndexes.begin());

            std::string sql_second = "insert into inputs (iid, mixin, value, anonset) values(\""
            + std::to_string(counter) + "\", \""
            + std::to_string(mixin-1) + "\", \""
            + std::to_string(amount) + "\", \""
            + std::to_string(anonset[amount]) + "\")";
            sql = const_cast<char*>(sql_second.c_str());
            rc = sqlite3_exec(dbs, sql, NULL, NULL, &zErrMsg);
            if (rc != SQLITE_OK) {
              fprintf(stderr, "SQL error: %s\n", zErrMsg);
              sqlite3_free(zErrMsg);
            }

            for (auto index_tracker = global_outputIndexes.begin(); index_tracker != global_outputIndexes.end(); ++index_tracker){
              //std::cout << "Input Amount: " << amount << '\n';
              //std::cout << "Global Index: " << *index_tracker << '\n';
              //std::cout << "From transaction: " << ref_tx << '\n';
              std::string sql_second = "insert into refs (iid, oid) values(\""
              + std::to_string(counter) + "\", \""
              + std::to_string(amount) + "-" + std::to_string(*index_tracker) + "\")";
              sql = const_cast<char*>(sql_second.c_str());
              rc = sqlite3_exec(dbs, sql, NULL, NULL, &zErrMsg);
              if (rc != SQLITE_OK) {
                fprintf(stderr, "SQL error: %s\n", zErrMsg);
                sqlite3_free(zErrMsg);
              }
            }
            counter++;
          }
        }

        /*  
            handle the outputs of the transaction over here.
            we get the amount output, global index and we update the 
            available mixins as well known as anonset
        */
        std::vector<TransactionOutputDetails> tx_outputs = tx_details->outputs;
        for (auto output_tracker = tx_outputs.begin(); output_tracker != tx_outputs.end(); ++output_tracker){
          uint64_t outglobalidx = output_tracker->globalIndex;
          uint64_t outamt = (output_tracker->output).amount;
          //std::cout << "Output Amount: " << outamt << '\n';
          //std::cout << "Global Index: " << outglobalidx << '\n';
          anonset[outamt] = outglobalidx;
          std::string sql_second = "insert into outputs (oid, amount, gidx) values(\""
          + std::to_string(outamt) + "-" + std::to_string(outglobalidx) + "\", \""
          + std::to_string(outamt) + "\", \""
          + std::to_string(outglobalidx) + "\")";

          sql = const_cast<char*>(sql_second.c_str());
          rc = sqlite3_exec(dbs, sql, NULL, NULL, &zErrMsg);
          if (rc != SQLITE_OK) {
            fprintf(stderr, "SQL error: %s\n", zErrMsg);
            sqlite3_free(zErrMsg);
          }
        }
      }
    }
    sqlite3_exec(dbs, "END TRANSACTION", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
    }
    else{
      fprintf(stdout, "Transaction committed successfully\n");
    }
    logger(INFO) << "I am stopping here, bye.";
    sqlite3_close(dbs);
    return 0;

    CryptoNote::CryptoNoteProtocolHandler cprotocol(currency, dispatcher, ccore, nullptr, logManager);
    CryptoNote::NodeServer p2psrv(dispatcher, cprotocol, logManager);
    CryptoNote::RpcServer rpcServer(dispatcher, logManager, ccore, p2psrv, cprotocol);

    cprotocol.set_p2p_endpoint(&p2psrv);
    DaemonCommandsHandler dch(ccore, p2psrv, logManager);
    logger(INFO) << "Initializing p2p server...";
    if (!p2psrv.init(netNodeConfig)) {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize p2p server.";
      return 1;
    }

    logger(INFO) << "P2p server initialized OK";

    if (!command_line::has_arg(vm, arg_console)) {
      dch.start_handling();
    }

    logger(INFO) << "Starting core rpc server on address " << rpcConfig.getBindAddress();
    rpcServer.start(rpcConfig.bindIp, rpcConfig.bindPort);
    logger(INFO) << "Core rpc server started ok";

    Tools::SignalHandler::install([&dch, &p2psrv] {
      dch.stop_handling();
      p2psrv.sendStopSignal();
    });

    logger(INFO) << "Starting p2p net loop...";
    p2psrv.run();
    logger(INFO) << "p2p net loop stopped";

    dch.stop_handling();

    //stop components
    logger(INFO) << "Stopping core rpc server...";
    rpcServer.stop();

    //deinitialize components
    logger(INFO) << "Deinitializing p2p...";
    p2psrv.deinit();

    cprotocol.set_p2p_endpoint(nullptr);
    ccore.save();

  } catch (const std::exception& e) {
    logger(ERROR, BRIGHT_RED) << "Exception: " << e.what();
    return 1;
  }

  logger(INFO) << "Node stopped.";
  return 0;
}

bool command_line_preprocessor(const boost::program_options::variables_map &vm, LoggerRef &logger) {
  bool exit = false;

  if (command_line::get_arg(vm, command_line::arg_version)) {
    std::cout << CryptoNote::CRYPTONOTE_NAME << " v" << PROJECT_VERSION_LONG << ENDL;
    exit = true;
  }
  if (command_line::get_arg(vm, arg_os_version)) {
    std::cout << "OS: " << Tools::get_os_version_string() << ENDL;
    exit = true;
  }

  if (exit) {
    return true;
  }

  return false;
}