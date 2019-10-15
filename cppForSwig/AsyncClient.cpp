
////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "AsyncClient.h"
#include "EncryptionUtils.h"
#include "BDVCodec.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/text_format.h"

using namespace std;
using namespace AsyncClient;
using namespace ::Codec_BDVCommand;

///////////////////////////////////////////////////////////////////////////////
//
// BlockDataViewer
//
///////////////////////////////////////////////////////////////////////////////
unique_ptr<WritePayload_Protobuf> BlockDataViewer::make_payload(
   Methods method, const string& bdvid)
{
   auto payload = make_unique<WritePayload_Protobuf>();
   auto message = make_unique<BDVCommand>();
   message->set_method(method);

   payload->message_ = move(message);
   return move(payload);
}

///////////////////////////////////////////////////////////////////////////////
unique_ptr<WritePayload_Protobuf> BlockDataViewer::make_payload(
   StaticMethods method)
{
   auto payload = make_unique<WritePayload_Protobuf>();
   auto message = make_unique<StaticCommand>();
   message->set_method(method);

   payload->message_ = move(message);
   return move(payload);
}

///////////////////////////////////////////////////////////////////////////////
bool BlockDataViewer::hasRemoteDB(void)
{
   return sock_->testConnection();
}

///////////////////////////////////////////////////////////////////////////////
bool BlockDataViewer::connectToRemote(void)
{
   return sock_->connectToRemote();
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::addPublicKey(const SecureBinaryData& pubkey)
{
   auto wsSock = dynamic_pointer_cast<WebSocketClient>(sock_);
   if (wsSock == nullptr)
   {
      LOGERR << "invalid socket type for auth peer management";
      return;
   }

   wsSock->addPublicKey(pubkey);
}

///////////////////////////////////////////////////////////////////////////////
shared_ptr<BlockDataViewer> BlockDataViewer::getNewBDV(const string& addr,
   const string& port, const string& datadir, const bool& ephemeralPeers,
   shared_ptr<RemoteCallback> callbackPtr)
{
   //create socket object
   auto sockptr = make_shared<WebSocketClient>(addr, port, datadir,
      ephemeralPeers, callbackPtr);

   //instantiate bdv object
   BlockDataViewer* bdvPtr = new BlockDataViewer(sockptr);

   //create shared_ptr of bdv object
   shared_ptr<BlockDataViewer> bdvSharedPtr;
   bdvSharedPtr.reset(bdvPtr);

   return bdvSharedPtr;
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::registerWithDB(BinaryData magic_word)
{
   if (bdvID_.size() != 0)
      throw BDVAlreadyRegistered();

   //get bdvID
   try
   {
      auto payload = make_payload(StaticMethods::registerBDV);
      auto command = dynamic_cast<StaticCommand*>(payload->message_.get());
      command->set_magicword(magic_word.getPtr(), magic_word.getSize());
      
      //registration is always blocking as it needs to guarantee the bdvID

      auto promPtr = make_shared<promise<string>>();
      auto fut = promPtr->get_future();
      auto getResult = [promPtr](ReturnMessage<string> result)->void
      {
         try
         {
            promPtr->set_value(move(result.get()));
         }
         catch (exception&)
         {
            auto eptr = current_exception();
            promPtr->set_exception(eptr);
         }
      };

      auto read_payload = make_shared<Socket_ReadPayload>();
      read_payload->callbackReturn_ =
         make_unique<CallbackReturn_String>(getResult);
      sock_->pushPayload(move(payload), read_payload);
 
      bdvID_ = move(fut.get());
   }
   catch (runtime_error &e)
   {
      LOGERR << e.what();
      throw e;
   }
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::unregisterFromDB()
{
   if (sock_->type() == SocketWS)
   {
      auto sockws = dynamic_pointer_cast<WebSocketClient>(sock_);
      if(sockws == nullptr)
         return;

      sockws->shutdown();
      return;
   }

   auto payload = make_payload(StaticMethods::unregisterBDV);
   sock_->pushPayload(move(payload), nullptr);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::goOnline()
{
   auto payload = make_payload(Methods::goOnline, bdvID_);
   sock_->pushPayload(move(payload), nullptr);
}

///////////////////////////////////////////////////////////////////////////////
BlockDataViewer::BlockDataViewer(void)
{
   cache_ = make_shared<ClientCache>();
}

///////////////////////////////////////////////////////////////////////////////
BlockDataViewer::BlockDataViewer(shared_ptr<SocketPrototype> sock) :
   sock_(sock)
{
   cache_ = make_shared<ClientCache>();
}

///////////////////////////////////////////////////////////////////////////////
BlockDataViewer::~BlockDataViewer()
{}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::shutdown(const string& cookie)
{
   auto payload = make_payload(StaticMethods::shutdown);
   auto command = dynamic_cast<StaticCommand*>(payload->message_.get());

   if (cookie.size() > 0)
      command->set_cookie(cookie);

   sock_->pushPayload(move(payload), nullptr);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::shutdownNode(const string& cookie)
{
   auto payload = make_payload(StaticMethods::shutdownNode);
   auto command = dynamic_cast<StaticCommand*>(payload->message_.get());

   if (cookie.size() > 0)
      command->set_cookie(cookie);

   sock_->pushPayload(move(payload), nullptr);
}

///////////////////////////////////////////////////////////////////////////////
AsyncClient::BtcWallet BlockDataViewer::instantiateWallet(const string& id)
{
   return BtcWallet(*this, id);
}

///////////////////////////////////////////////////////////////////////////////
Lockbox BlockDataViewer::instantiateLockbox(const string& id)
{
   return Lockbox(*this, id);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getLedgerDelegateForWallets(
   function<void(ReturnMessage<LedgerDelegate>)> callback)
{
   auto payload = make_payload(Methods::getLedgerDelegateForWallets, bdvID_);
   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_LedgerDelegate>(sock_, bdvID_, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getLedgerDelegateForLockboxes(
   function<void(ReturnMessage<LedgerDelegate>)> callback)
{
   auto payload = make_payload(Methods::getLedgerDelegateForLockboxes, bdvID_);
   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_LedgerDelegate>(sock_, bdvID_, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
AsyncClient::Blockchain BlockDataViewer::blockchain(void)
{
   return Blockchain(*this);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::broadcastZC(const BinaryData& rawTx)
{
   auto&& txHash = BtcUtils::getHash256(rawTx.getRef());
   Tx tx(rawTx);
   cache_->insertTx(txHash, tx);

   auto payload = make_payload(Methods::broadcastZC, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->add_bindata(rawTx.getPtr(), rawTx.getSize());

   sock_->pushPayload(move(payload), nullptr);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getTxByHash(const BinaryData& txHash, 
   function<void(ReturnMessage<Tx>)> callback)
{
   BinaryDataRef bdRef(txHash);
   BinaryData hash;

   if (txHash.getSize() != 32)
   {
      if (txHash.getSize() == 64)
      {
         string hashstr(txHash.toCharPtr(), txHash.getSize());
         hash = READHEX(hashstr);
         bdRef.setRef(hash);
      }
   }

   bool heightOnly = false;
   try
   {
      auto& tx = cache_->getTx(bdRef);
      if (tx.getTxHeight() == UINT32_MAX)
      {
         //Throw out of this scope if the tx is cached but lacks a valid height.
         //Flag to only fetch the height as well.
         heightOnly = true;
         throw NoMatch();
      }

      /*
      We have this tx in cache, bypass the db and trigger the callback directly.
      
      This is an async interface, the expectation is that the callback will be 
      summoned from a different thread than the original call.

      Moreover, this framework always triggers return value callbacks in their own
      dedicated thread, as it does not expect users to treat the callback as a
      short-lived notification to return quickly from.

      Therefor, it is always acceptable to create a new thread to fire the 
      callback from.
      */
      ReturnMessage<Tx> rm(tx);

      thread thr(callback, move(rm));
      if (thr.joinable())
         thr.detach();

      return;
   }
   catch(NoMatch&)
   {}

   auto payload = make_payload(Methods::getTxByHash, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_hash(bdRef.getPtr(), bdRef.getSize());
   command->set_flag(heightOnly);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_Tx>(cache_, txHash, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getTxBatchByHash(const set<BinaryData>& hashes,
   function<void(ReturnMessage<vector<Tx>>)> callback)
{
   //only accepts hashes in binary format
   auto payload = make_payload(Methods::getTxBatchByHash, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   std::map<BinaryData, bool> hashesToFetch;
   std::vector<Tx> cachedTx;
   for (auto& hash : hashes)
   {
      try
      {
         auto& tx = cache_->getTx(hash.getRef());

         //flag to grab only the txheight if it's unset
         if(tx.getTxHeight() == UINT32_MAX)
            hashesToFetch.insert(make_pair(hash, true));
         else
            cachedTx.push_back(tx);

         continue;
      }
      catch (NoMatch&)
      {}

      hashesToFetch.insert(make_pair(hash, false));
   }
      
   if (hashesToFetch.size() == 0)
   {
      //all tx in cache, fire the callback
      ReturnMessage<vector<Tx>> rm(move(cachedTx));
      thread thr(callback, move(rm));
      if (thr.joinable())
         thr.detach();

      return;
   }
   
   for (auto& hash : hashesToFetch)
   {
      if (!hash.second)
      {
         command->add_bindata(hash.first.getPtr(), hash.first.getSize());
      }
      else
      {
         BinaryWriter bw(33);
         bw.put_BinaryDataRef(hash.first);
         bw.put_uint8_t(1);
         command->add_bindata(bw.getDataRef().getPtr(), 33);
      }
   }

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_TxBatch>(
         cache_, cachedTx, hashesToFetch, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getRawHeaderForTxHash(const BinaryData& txHash,
   function<void(ReturnMessage<BinaryData>)> callback)
{
   BinaryDataRef bdRef(txHash);
   BinaryData hash;

   if (txHash.getSize() != 32)
   {
      if (txHash.getSize() == 64)
      {
         string hashstr(txHash.toCharPtr(), txHash.getSize());
         hash = READHEX(hashstr);
         bdRef.setRef(hash);
      }
   }

   try
   {
      auto& height = cache_->getHeightForTxHash(txHash);
      auto& rawHeader = cache_->getRawHeader(height);
      callback(rawHeader);
      return;
   }
   catch(NoMatch&)
   { }

   auto payload = make_payload(Methods::getHeaderByHash, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->add_bindata(txHash.getPtr(), txHash.getSize());

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_RawHeader>(
         cache_, UINT32_MAX, txHash, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getHeaderByHeight(unsigned height,
   function<void(ReturnMessage<BinaryData>)> callback)
{
   try
   {
      auto& rawHeader = cache_->getRawHeader(height);
      callback(rawHeader);
      return;
   }
   catch(NoMatch&)
   { }

   auto payload = make_payload(Methods::getHeaderByHeight, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_height(height);

   BinaryData txhash;
   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_RawHeader>(
         cache_, height, txhash, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getLedgerDelegateForScrAddr(
   const string& walletID, const BinaryData& scrAddr,
   function<void(ReturnMessage<LedgerDelegate>)> callback)
{
   auto payload = make_payload(Methods::getLedgerDelegateForScrAddr, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID);
   command->set_scraddr(scrAddr.getPtr(), scrAddr.getSize());

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_LedgerDelegate>(sock_, bdvID_, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::updateWalletsLedgerFilter(
   const vector<BinaryData>& wltIdVec)
{
   auto payload = make_payload(Methods::updateWalletsLedgerFilter, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   for (auto bd : wltIdVec)
      command->add_bindata(bd.getPtr(), bd.getSize());

   sock_->pushPayload(move(payload), nullptr);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getNodeStatus(
   function<void(
      ReturnMessage<shared_ptr<::ClientClasses::NodeStatusStruct>>)> callback)
{
   auto payload = make_payload(Methods::getNodeStatus, bdvID_);
   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_NodeStatusStruct>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::estimateFee(unsigned blocksToConfirm, 
   const string& strategy, 
   function<void(ReturnMessage<ClientClasses::FeeEstimateStruct>)> callback)
{
   auto payload = make_payload(Methods::estimateFee, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_value(blocksToConfirm);
   command->add_bindata(strategy);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_FeeEstimateStruct>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getFeeSchedule(const string& strategy, function<void(
   ReturnMessage<map<unsigned, ClientClasses::FeeEstimateStruct>>)> callback)
{
   auto payload = make_payload(Methods::getFeeSchedule, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->add_bindata(strategy);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_FeeSchedule>(callback);
   sock_->pushPayload(move(payload), read_payload);
}


///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getHistoryForWalletSelection(
   const vector<string>& wldIDs, const string& orderingStr,
   function<void(ReturnMessage<vector<::ClientClasses::LedgerEntry>>)> callback)
{
   auto payload = make_payload(Methods::getHistoryForWalletSelection, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   if (orderingStr == "ascending")
      command->set_flag(true);
   else if (orderingStr == "descending")
      command->set_flag(false);
   else
      throw runtime_error("invalid ordering string");

   for (auto& id : wldIDs)
      command->add_bindata(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorLedgerEntry>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::broadcastThroughRPC(const BinaryData& rawTx,
   function<void(ReturnMessage<string>)> callback)
{
   auto payload = make_payload(Methods::broadcastThroughRPC, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->add_bindata(rawTx.getPtr(), rawTx.getSize());

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_String>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::getSpentnessForOutputs(
   const map<BinaryData, set<unsigned>>& outputs,
   std::function<void(ReturnMessage<
      std::map<BinaryData, std::map<unsigned, BinaryData>>>)> callback)
{
   auto payload = make_payload(Methods::getSpentnessForOutputs, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& hashPair : outputs)
   {
      BinaryWriter bw;
      bw.put_BinaryData(hashPair.first);
      bw.put_var_int(hashPair.second.size());
      
      for (auto& id : hashPair.second)
         bw.put_var_int(id);

      command->add_bindata(bw.getDataRef().getPtr(), bw.getSize());
   }

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_SpentnessData>(outputs, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void BlockDataViewer::setCheckServerKeyPromptLambda(
   function<bool(const BinaryData&, const string&)> lbd)
{
   auto wsSock = dynamic_pointer_cast<WebSocketClient>(sock_);
   if (wsSock == nullptr)
      return;

   wsSock->setPubkeyPromptLambda(lbd);
}

///////////////////////////////////////////////////////////////////////////////
//
// LedgerDelegate
//
///////////////////////////////////////////////////////////////////////////////
LedgerDelegate::LedgerDelegate(shared_ptr<SocketPrototype> sock,
   const string& bdvid, const string& ldid) :
   delegateID_(ldid), bdvID_(bdvid), sock_(sock)
{}

///////////////////////////////////////////////////////////////////////////////
void LedgerDelegate::getHistoryPage(uint32_t id, 
   function<void(ReturnMessage<vector<::ClientClasses::LedgerEntry>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getHistoryPage, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_delegateid(delegateID_);
   command->set_pageid(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorLedgerEntry>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void LedgerDelegate::getPageCount(
   function<void(ReturnMessage<uint64_t>)> callback) const
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getPageCountForLedgerDelegate, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_delegateid(delegateID_);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_UINT64>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
//
// BtcWallet
//
///////////////////////////////////////////////////////////////////////////////
AsyncClient::BtcWallet::BtcWallet(const BlockDataViewer& bdv, const string& id) :
   walletID_(id), bdvID_(bdv.bdvID_), sock_(bdv.sock_)
{}

///////////////////////////////////////////////////////////////////////////////
string AsyncClient::BtcWallet::registerAddresses(
   const vector<BinaryData>& addrVec, bool isNew)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::registerWallet, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_flag(isNew);
   command->set_walletid(walletID_);

   auto&& registrationId = CryptoPRNG::generateRandom(5).toHexStr();
   command->set_hash(registrationId);

   for (auto& addr : addrVec)
      command->add_bindata(addr.getPtr(), addr.getSize());
   sock_->pushPayload(move(payload), nullptr);

   return registrationId;
}

///////////////////////////////////////////////////////////////////////////////
string AsyncClient::BtcWallet::setUnconfirmedTarget(unsigned confTarget)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::setWalletConfTarget, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);

   auto&& registrationId = CryptoPRNG::generateRandom(5).toHexStr();
   command->set_hash(registrationId);
   command->set_height(confTarget);

   sock_->pushPayload(move(payload), nullptr);
   return registrationId;
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getBalancesAndCount(uint32_t blockheight, 
   function<void(ReturnMessage<vector<uint64_t>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getBalancesAndCount, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);
   command->set_height(blockheight);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUINT64>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getSpendableTxOutListForValue(uint64_t val,
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getSpendableTxOutListForValue, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);
   command->set_value(val);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getSpendableZCList(
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getSpendableZCList, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getRBFTxOutList(
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getRBFTxOutList, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getAddrTxnCountsFromDB(
   function<void(ReturnMessage<map<BinaryData, uint32_t>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getAddrTxnCounts, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_Map_BD_U32>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getAddrBalancesFromDB(
   function<void(ReturnMessage<map<BinaryData, vector<uint64_t>>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getAddrBalances, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_Map_BD_VecU64>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getHistoryPage(uint32_t id,
   function<void(ReturnMessage<vector<::ClientClasses::LedgerEntry>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getHistoryPage, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);
   command->set_pageid(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorLedgerEntry>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::getLedgerEntryForTxHash(
   const BinaryData& txhash, 
   function<void(ReturnMessage<shared_ptr<::ClientClasses::LedgerEntry>>)> callback)
{  
   //get history page with a hash as argument instead of an int will return 
   //the ledger entry for the tx instead of a page

   auto payload = BlockDataViewer::make_payload(
      Methods::getHistoryPage, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);
   command->set_hash(txhash.getPtr(), txhash.getSize());

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_LedgerEntry>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
ScrAddrObj AsyncClient::BtcWallet::getScrAddrObjByKey(const BinaryData& scrAddr,
   uint64_t full, uint64_t spendable, uint64_t unconf, uint32_t count)
{
   return ScrAddrObj(sock_, bdvID_, walletID_, scrAddr, INT32_MAX,
      full, spendable, unconf, count);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BtcWallet::createAddressBook(
   function<void(ReturnMessage<vector<AddressBookEntry>>)> callback) const
{
   auto payload = BlockDataViewer::make_payload(
      Methods::createAddressBook, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorAddressBookEntry>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
//
// Lockbox
//
///////////////////////////////////////////////////////////////////////////////
void Lockbox::getBalancesAndCountFromDB(uint32_t topBlockHeight)
{
   auto setValue = [this](ReturnMessage<vector<uint64_t>> int_vec)->void
   {
      auto v = move(int_vec.get());
      if (v.size() != 4)
         throw runtime_error("unexpected vector size");

      fullBalance_ = v[0];
      spendableBalance_ = v[1];
      unconfirmedBalance_ = v[2];

      txnCount_ = v[3];
   };

   BtcWallet::getBalancesAndCount(topBlockHeight, setValue);
}

///////////////////////////////////////////////////////////////////////////////
string AsyncClient::Lockbox::registerAddresses(
   const vector<BinaryData>& addrVec, bool isNew)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::registerLockbox, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_flag(isNew);
   command->set_walletid(walletID_);
   
   auto&& registrationId = CryptoPRNG::generateRandom(5).toHexStr();
   command->set_hash(registrationId);

   for (auto& addr : addrVec)
      command->add_bindata(addr.getPtr(), addr.getSize());
   sock_->pushPayload(move(payload), nullptr);

   return registrationId;
}

///////////////////////////////////////////////////////////////////////////////
//
// ScrAddrObj
//
///////////////////////////////////////////////////////////////////////////////
ScrAddrObj::ScrAddrObj(shared_ptr<SocketPrototype> sock, const string& bdvId,
   const string& walletID, const BinaryData& scrAddr, int index,
   uint64_t full, uint64_t spendabe, uint64_t unconf, uint32_t count) :
   bdvID_(bdvId), walletID_(walletID), scrAddr_(scrAddr), sock_(sock),
   fullBalance_(full), spendableBalance_(spendabe),
   unconfirmedBalance_(unconf), count_(count), index_(index)
{}

///////////////////////////////////////////////////////////////////////////////
ScrAddrObj::ScrAddrObj(AsyncClient::BtcWallet* wlt, const BinaryData& scrAddr,
   int index, uint64_t full, uint64_t spendabe, uint64_t unconf, uint32_t count) :
   bdvID_(wlt->bdvID_), walletID_(wlt->walletID_), scrAddr_(scrAddr), sock_(wlt->sock_),
   fullBalance_(full), spendableBalance_(spendabe),
   unconfirmedBalance_(unconf), count_(count), index_(index)
{}

///////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::getSpendableTxOutList(bool ignoreZC, 
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getSpendableTxOutListForAddr, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_walletid(walletID_);
   command->set_scraddr(scrAddr_.getPtr(), scrAddr_.getSize());

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
//
// Blockchain
//
///////////////////////////////////////////////////////////////////////////////
AsyncClient::Blockchain::Blockchain(const BlockDataViewer& bdv) :
   sock_(bdv.sock_), bdvID_(bdv.bdvID_)
{}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::Blockchain::getHeaderByHash(const BinaryData& hash,
   function<void(ReturnMessage<ClientClasses::BlockHeader>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getHeaderByHash, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_hash(hash.getPtr(), hash.getSize());

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_BlockHeader>(UINT32_MAX, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::Blockchain::getHeaderByHeight(unsigned height,
   function<void(ReturnMessage<ClientClasses::BlockHeader>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getHeaderByHeight, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());
   command->set_height(height);
   
   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_BlockHeader>(height, callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
pair<unsigned, unsigned> AsyncClient::BlockDataViewer::getRekeyCount() const
{
   auto wsSocket = dynamic_pointer_cast<WebSocketClient>(sock_);
   if (wsSocket == nullptr)
      return make_pair(0, 0);

   return wsSocket->getRekeyCount();
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getCombinedBalances(
   const vector<string>& wltIDs,
   function<void(ReturnMessage<map<string, CombinedBalances>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getCombinedBalances, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& id : wltIDs)
      command->add_bindata(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ = 
      make_unique<CallbackReturn_CombinedBalances>(callback);   
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getCombinedAddrTxnCounts(
   const vector<string>& wltIDs,
   function<void(ReturnMessage<map<string, CombinedCounts>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getCombinedAddrTxnCounts, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& id : wltIDs)
      command->add_bindata(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ = 
      make_unique<CallbackReturn_CombinedCounts>(callback);   
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getCombinedSpendableTxOutListForValue(
   const vector<string>& wltIDs, uint64_t value,
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getCombinedSpendableTxOutListForValue, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& id : wltIDs)
      command->add_bindata(id);

   command->set_value(value);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ = 
      make_unique<CallbackReturn_VectorUTXO>(callback);   
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getCombinedSpendableZcOutputs(
   const vector<string>& wltIDs, 
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getCombinedSpendableZcOutputs, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& id : wltIDs)
      command->add_bindata(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getCombinedRBFTxOuts(
   const vector<string>& wltIDs,
   function<void(ReturnMessage<vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getCombinedRBFTxOuts, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& id : wltIDs)
      command->add_bindata(id);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getOutpointsForAddresses(
   const std::set<BinaryData>& addrVec, 
   unsigned startHeight, unsigned zcIndexCutoff, 
   std::function<void(ReturnMessage<OutpointBatch>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getOutpointsForAddresses, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   for (auto& id : addrVec)
      command->add_bindata(id.getCharPtr(), id.getSize());

   command->set_height(startHeight);
   command->set_zcid(zcIndexCutoff);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_AddrOutpoints>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
void AsyncClient::BlockDataViewer::getUTXOsForAddress(
   const BinaryData& scrAddr, bool withZc,
   std::function<void(ReturnMessage<std::vector<UTXO>>)> callback)
{
   auto payload = BlockDataViewer::make_payload(
      Methods::getUTXOsForAddress, bdvID_);
   auto command = dynamic_cast<BDVCommand*>(payload->message_.get());

   command->set_scraddr(scrAddr.getCharPtr(), scrAddr.getSize());
   command->set_flag(withZc);

   auto read_payload = make_shared<Socket_ReadPayload>();
   read_payload->callbackReturn_ =
      make_unique<CallbackReturn_VectorUTXO>(callback);
   sock_->pushPayload(move(payload), read_payload);
}

///////////////////////////////////////////////////////////////////////////////
//
// CallbackReturn children
//
///////////////////////////////////////////////////////////////////////////////
void AsyncClient::deserialize(
   google::protobuf::Message* ptr, const WebSocketMessagePartial& partialMsg)
{
   if (!partialMsg.getMessage(ptr))
   {
      ::Codec_NodeStatus::BDV_Error errorMsg;
      if (!partialMsg.getMessage(&errorMsg))
         throw ClientMessageError("unknown error deserializing message");

      throw ClientMessageError(errorMsg.error());
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_BinaryDataRef::callback(
   const WebSocketMessagePartial& partialMsg)
{
   auto msg = make_shared<::Codec_CommonTypes::BinaryData>();
   AsyncClient::deserialize(msg.get(), partialMsg);

   auto lbd = [this, msg](void)->void
   {
      auto str = msg->data();
      BinaryDataRef ref;
      ref.setRef(str);
      userCallbackLambda_(ref);
   };

   if (runInCaller())
   {
      lbd();
   }
   else
   {
      thread thr(lbd);
      if (thr.joinable())
         thr.detach();
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_String::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::Strings msg;
      AsyncClient::deserialize(&msg, partialMsg);

      if (msg.data_size() != 1)
         throw ClientMessageError("invalid message in CallbackReturn_String");

      auto str = msg.data(0);
      ReturnMessage<string> rm(str);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<string> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_LedgerDelegate::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::Strings msg;
      AsyncClient::deserialize(&msg, partialMsg);

      if (msg.data_size() != 1)
         throw ClientMessageError("invalid message in CallbackReturn_LedgerDelegate");

      auto& str = msg.data(0);

      LedgerDelegate ld(sockPtr_, bdvID_, str);
      ReturnMessage<LedgerDelegate> rm(ld);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<LedgerDelegate> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_Tx::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::TxWithMetaData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      Tx tx;
      if (msg.has_rawtx())
      {
         auto& rawtx = msg.rawtx();
         BinaryDataRef ref;
         ref.setRef(rawtx);

         tx.unserialize(ref);
         tx.setChainedZC(msg.ischainedzc());
         tx.setRBF(msg.isrbf());
         tx.setTxHeight(msg.height());
         tx.setTxIndex(msg.txindex());
         cache_->insertTx(txHash_, tx);
      }
      else
      {
         try {
            auto& cachedTx = cache_->getTx(txHash_.getRef());
            cachedTx.setTxHeight(msg.height());
            cachedTx.setTxIndex(msg.txindex());
            tx = cachedTx;
         } catch (NoMatch) {
         }
      }
      
      ReturnMessage<Tx> rm(move(tx));

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<Tx> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_TxBatch::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::ManyTxWithMetaData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      auto iter = callMap_.begin();
      for (unsigned i = 0; i < msg.tx_size(); i++)
      {
         Tx tx;
         auto& txHash = iter->first;
         auto& txObj = msg.tx(i);

         if (txObj.has_rawtx() && !iter->second)
         {
            BinaryDataRef ref;
            ref.setRef(txObj.rawtx());

            tx.unserialize(ref);
            tx.setChainedZC(txObj.ischainedzc());
            tx.setRBF(txObj.isrbf());
            tx.setTxHeight(txObj.height());
            tx.setTxIndex(txObj.txindex());
            
            cache_->insertTx(txHash, tx);
         }
         else
         {
            auto& txFromCache = cache_->getTx(txHash);
            txFromCache.setTxHeight(txObj.height());
            txFromCache.setTxIndex(txObj.txindex());
            tx = txFromCache;
         }

         cachedTx_.emplace_back(tx);
         ++iter;
      }

      //order the tx vector before returning it
      std::sort(cachedTx_.begin(), cachedTx_.end(), TxComparator());

      ReturnMessage<vector<Tx>> rm(move(cachedTx_));

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<vector<Tx>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_RawHeader::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::BinaryData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      auto& rawheader = msg.data();
      BinaryDataRef ref; ref.setRef(rawheader);
      BinaryRefReader brr(ref);
      auto&& header = brr.get_BinaryData(HEADER_SIZE);

      if (height_ == UINT32_MAX)
         height_ = brr.get_uint32_t();

      if (txHash_.getSize() != 0)
         cache_->insertHeightForTxHash(txHash_, height_);
      cache_->insertRawHeader(height_, header);

      ReturnMessage<BinaryData> rm(header);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<BinaryData> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_NodeStatusStruct::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      auto msg = make_shared<::Codec_NodeStatus::NodeStatus>();
      AsyncClient::deserialize(msg.get(), partialMsg);

      auto nss = make_shared<::ClientClasses::NodeStatusStruct>(msg);
      
      ReturnMessage<shared_ptr<::ClientClasses::NodeStatusStruct>> rm(nss);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<shared_ptr<::ClientClasses::NodeStatusStruct>> rm(e);
      userCallbackLambda_(rm);
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_FeeEstimateStruct::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_FeeEstimate::FeeEstimate msg;
      AsyncClient::deserialize(&msg, partialMsg);

      ClientClasses::FeeEstimateStruct fes(
         msg.feebyte(), msg.smartfee(), msg.error());

      ReturnMessage<ClientClasses::FeeEstimateStruct> rm(fes);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<ClientClasses::FeeEstimateStruct> rm(e);
      userCallbackLambda_(rm);
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_FeeSchedule::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_FeeEstimate::FeeSchedule msg;
      AsyncClient::deserialize(&msg, partialMsg);

      map<unsigned, ClientClasses::FeeEstimateStruct> result;

      for (int i = 0; i < msg.estimate_size(); i++)
      {
         auto& feeByte = msg.estimate(i);
         ClientClasses::FeeEstimateStruct fes(
            feeByte.feebyte(), feeByte.smartfee(), feeByte.error());

         auto target = msg.target(i);
         result.insert(make_pair(target, move(fes)));
      }

      ReturnMessage<map<unsigned, ClientClasses::FeeEstimateStruct>> rm(result);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<map<unsigned, ClientClasses::FeeEstimateStruct>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_VectorLedgerEntry::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      auto msg = make_shared<::Codec_LedgerEntry::ManyLedgerEntry>();
      AsyncClient::deserialize(msg.get(), partialMsg);


      vector<::ClientClasses::LedgerEntry> lev;

      for (int i = 0; i < msg->values_size(); i++)
      {
         ::ClientClasses::LedgerEntry le(msg, i);
         lev.push_back(move(le));
      }

      ReturnMessage<vector<::ClientClasses::LedgerEntry>> rm(lev);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<vector<::ClientClasses::LedgerEntry>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_UINT64::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::OneUnsigned msg;
      AsyncClient::deserialize(&msg, partialMsg);

      uint64_t result = msg.value();

      ReturnMessage<uint64_t> rm(result);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<uint64_t> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_VectorUTXO::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_Utxo::ManyUtxo utxos;
      AsyncClient::deserialize(&utxos, partialMsg);

      vector<UTXO> utxovec(utxos.value_size());
      for (int i = 0; i < utxos.value_size(); i++)
      {
         auto& proto_utxo = utxos.value(i);
         auto& utxo = utxovec[i];

         utxo.value_ = proto_utxo.value();
         utxo.script_.copyFrom(proto_utxo.script());
         utxo.txHeight_ = proto_utxo.txheight();
         utxo.txIndex_ = proto_utxo.txindex();
         utxo.txOutIndex_ = proto_utxo.txoutindex();
         utxo.txHash_.copyFrom(proto_utxo.txhash());
      }

      ReturnMessage<vector<UTXO>> rm(utxovec);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<vector<UTXO>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_VectorUINT64::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::ManyUnsigned msg;
      AsyncClient::deserialize(&msg, partialMsg);

      vector<uint64_t> intvec(msg.value_size());
      for (int i = 0; i < msg.value_size(); i++)
         intvec[i] = msg.value(i);

      ReturnMessage<vector<uint64_t>> rm(intvec);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<vector<uint64_t>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_Map_BD_U32::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_AddressData::ManyAddressData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      map<BinaryData, uint32_t> bdmap;

      for (int i = 0; i < msg.scraddrdata_size(); i++)
      {
         auto& addrData = msg.scraddrdata(i);
         auto& addr = addrData.scraddr();
         BinaryDataRef addrRef;
         addrRef.setRef(addr);

         if (addrData.value_size() != 1)
            throw runtime_error("invalid msg for CallbackReturn_Map_BD_U32");

         bdmap[addrRef] = addrData.value(0);
      }

      ReturnMessage<map<BinaryData, uint32_t>> rm(bdmap);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<map<BinaryData, uint32_t>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_Map_BD_VecU64::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_AddressData::ManyAddressData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      map<BinaryData, vector<uint64_t>> bdMap;
      for (int i = 0; i < msg.scraddrdata_size(); i++)
      {
         auto& addrData = msg.scraddrdata(i);
         auto& addr = addrData.scraddr();
         BinaryDataRef addrRef;
         addrRef.setRef(addr);
         auto& vec = bdMap[addrRef];

         for (int y = 0; y < addrData.value_size(); y++)
            vec.push_back(addrData.value(y));
      }

      ReturnMessage<map<BinaryData, vector<uint64_t>>> rm(bdMap);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<map<BinaryData, vector<uint64_t>>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_LedgerEntry::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      auto msg = make_shared<::Codec_LedgerEntry::LedgerEntry>();
      AsyncClient::deserialize(msg.get(), partialMsg);

      auto le = make_shared<::ClientClasses::LedgerEntry>(msg);

      ReturnMessage<shared_ptr<::ClientClasses::LedgerEntry>> rm(le);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<shared_ptr<::ClientClasses::LedgerEntry>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_VectorAddressBookEntry::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_AddressBook::AddressBook addressBook;
      AsyncClient::deserialize(&addressBook, partialMsg);

      vector<AddressBookEntry> abVec;
      for (int i = 0; i < addressBook.entry_size(); i++)
      {
         auto& entry = addressBook.entry(i);
         AddressBookEntry abe;
         abe.scrAddr_.copyFrom(entry.scraddr());

         for (int y = 0; y < entry.txhash_size(); y++)
         {
            BinaryData bd(entry.txhash(y));
            abe.txHashList_.push_back(move(bd));
         }

         abVec.push_back(move(abe));
      }

      ReturnMessage<vector<AddressBookEntry>> rm(abVec);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<vector<AddressBookEntry>> rm(e);
      userCallbackLambda_(move(rm));
   }
}
///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_Bool::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::OneUnsigned msg;
      AsyncClient::deserialize(&msg, partialMsg);

      ReturnMessage<bool> rm(msg.value());

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<bool> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_BlockHeader::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_CommonTypes::BinaryData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      auto& str = msg.data();
      BinaryDataRef ref;
      ref.setRef(str);

      ClientClasses::BlockHeader bh(ref, height_);

      ReturnMessage<ClientClasses::BlockHeader> rm(bh);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<ClientClasses::BlockHeader> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_BDVCallback::callback(
   const WebSocketMessagePartial& partialMsg)
{
   auto msg = make_shared<::Codec_BDVCommand::BDVCallback>();
   AsyncClient::deserialize(msg.get(), partialMsg);

   userCallbackLambda_(msg);
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_CombinedBalances::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_AddressData::ManyCombinedData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      map<string, CombinedBalances> result;

      for (unsigned i=0; i<msg.packedbalance_size(); i++)
      {
         auto wltVals = msg.packedbalance(i);

         CombinedBalances cbal;
         cbal.walletId_ = wltVals.id();

         for (unsigned y=0; y<wltVals.idbalances_size(); y++)
            cbal.walletBalanceAndCount_.push_back(wltVals.idbalances(y));

         for (unsigned y=0; y<wltVals.addrdata_size(); y++)
         {
            auto addrBals = wltVals.addrdata(y);
               
            BinaryData scrAddr(addrBals.scraddr());

            vector<uint64_t> abl;
            for (unsigned z=0; z<addrBals.value_size(); z++)
               abl.push_back(addrBals.value(z));

            cbal.addressBalances_.insert(make_pair(scrAddr, abl));
         }

         result.insert(make_pair(wltVals.id(), cbal));
      }
      
      ReturnMessage<map<string, CombinedBalances>> rm(result);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<map<string, CombinedBalances>> rm(e);
      userCallbackLambda_(move(rm));
   }  
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_CombinedCounts::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_AddressData::ManyCombinedData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      map<string, CombinedCounts> result;

      for (unsigned i=0; i<msg.packedbalance_size(); i++)
      {
         auto wltVals = msg.packedbalance(i);

         CombinedCounts cbal;
         cbal.walletId_ = wltVals.id();

         for (unsigned y=0; y<wltVals.addrdata_size(); y++)
         {
            auto addrBals = wltVals.addrdata(y);
               
            BinaryData scrAddr(addrBals.scraddr());

            uint64_t bl = addrBals.value(0);
            cbal.addressTxnCounts_.insert(make_pair(scrAddr, bl));
         }

         result.insert(make_pair(wltVals.id(), cbal));
      }
      
      ReturnMessage<map<string, CombinedCounts>> rm(result);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<map<string, CombinedCounts>> rm(e);
      userCallbackLambda_(move(rm));
   }  
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_AddrOutpoints::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      ::Codec_Utxo::AddressOutpointsData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      OutpointBatch result;
      result.heightCutoff_ = msg.heightcutoff();
      result.zcIndexCutoff_ = msg.zcindexcutoff();

      for (unsigned i = 0; i < msg.addroutpoints_size(); i++)
      {
         auto& addrOutpoints = msg.addroutpoints(i);
         BinaryData scrAddr(addrOutpoints.scraddr());

         vector<OutpointData> outpointVec(addrOutpoints.outpoints_size());
         for (unsigned y = 0; y < addrOutpoints.outpoints_size(); y++)
         {
            auto& outpoint = addrOutpoints.outpoints(y);
            auto& opData = outpointVec[y];

            opData.value_ = outpoint.value();
            opData.txHeight_ = outpoint.txheight();
            opData.txOutIndex_ = outpoint.txoutindex();
            opData.txHash_.copyFrom(outpoint.txhash());
            opData.isSpent_ = outpoint.isspent();
            opData.txIndex_ = outpoint.txindex();
            if(opData.isSpent_)
               opData.spenderHash_ = outpoint.spenderhash();
         }

         result.outpoints_.insert(make_pair(scrAddr, outpointVec));
      }

      ReturnMessage<OutpointBatch> rm(result);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<OutpointBatch> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
void CallbackReturn_SpentnessData::callback(
   const WebSocketMessagePartial& partialMsg)
{
   try
   {
      unsigned i = 0;

      ::Codec_CommonTypes::ManyBinaryData msg;
      AsyncClient::deserialize(&msg, partialMsg);

      map<BinaryData, map<unsigned, BinaryData>> result;
      for (auto& hashPair : outputs_)
      {
         auto iter = result.insert(
            make_pair(hashPair.first, map<unsigned, BinaryData>())).first;

         for (auto& id : hashPair.second)
         {
            auto& val = msg.value(i++);
            BinaryDataRef hashRef; hashRef.setRef(val.data());

            iter->second.insert(make_pair(id, hashRef));
         }
      }

      ReturnMessage<map<BinaryData, map<unsigned, BinaryData>>> rm(result);

      if (runInCaller())
      {
         userCallbackLambda_(move(rm));
      }
      else
      {
         thread thr(userCallbackLambda_, move(rm));
         if (thr.joinable())
            thr.detach();
      }
   }
   catch (ClientMessageError& e)
   {
      ReturnMessage<map<BinaryData, map<unsigned, BinaryData>>> rm(e);
      userCallbackLambda_(move(rm));
   }
}

///////////////////////////////////////////////////////////////////////////////
//
// ClientCache
//
///////////////////////////////////////////////////////////////////////////////
void ClientCache::insertTx(const BinaryData& hash, const Tx& tx)
{
   ReentrantLock(this);
   txMap_.insert(make_pair(hash, tx));
}

///////////////////////////////////////////////////////////////////////////////
void ClientCache::insertRawHeader(unsigned& height, BinaryDataRef header)
{
   ReentrantLock(this);
   rawHeaderMap_.insert(make_pair(height, header));
}

///////////////////////////////////////////////////////////////////////////////
void ClientCache::insertHeightForTxHash(BinaryData& hash, unsigned& height)
{
   ReentrantLock(this);
   txHashToHeightMap_.insert(make_pair(hash, height));
}

///////////////////////////////////////////////////////////////////////////////
const Tx& ClientCache::getTx(const BinaryDataRef& hashRef) const
{
   ReentrantLock(this);

   auto iter = txMap_.find(hashRef);
   if (iter == txMap_.end())
      throw NoMatch();

   return iter->second;
}

///////////////////////////////////////////////////////////////////////////////
const BinaryData& ClientCache::getRawHeader(const unsigned& height) const
{
   ReentrantLock(this);

   auto iter = rawHeaderMap_.find(height);
   if (iter == rawHeaderMap_.end())
      throw NoMatch();

   return iter->second;
}

///////////////////////////////////////////////////////////////////////////////
const unsigned& ClientCache::getHeightForTxHash(const BinaryData& height) const
{
   ReentrantLock(this);

   auto iter = txHashToHeightMap_.find(height);
   if (iter == txHashToHeightMap_.end())
      throw NoMatch();

   return iter->second;
}

