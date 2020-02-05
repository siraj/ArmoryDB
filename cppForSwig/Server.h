////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-18, goatpig.                                           //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _SERVER_H_
#define _SERVER_H_

#include <string.h>
#include <string>
#include <memory>
#include <atomic>
#include <vector>

#include "WebSocketMessage.h"
#include "libwebsockets.h"

#include "ThreadSafeClasses.h"
#include "BDV_Notification.h"
#include "BinaryData.h"
#include "EncryptionUtils.h"
#include "BlockDataManagerConfig.h"
#include "SocketService.h"
#include "AuthorizedPeers.h"

#include "BIP150_151.h"
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#define SERVER_AUTH_PEER_FILENAME "server.peers"

class Clients;
class BlockDataManagerThread;

///////////////////////////////////////////////////////////////////////////////
struct per_session_data__http {
   lws_fop_fd_t fop_fd;
};

struct per_session_data__bdv {
   static const unsigned rcv_size = 8000;
   uint64_t id_;
};

enum demo_protocols {
   /* always first */
   PROTOCOL_HTTP = 0,

   PROTOCOL_ARMORY_BDM,

   /* always last */
   DEMO_PROTOCOL_COUNT
};

int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user,
   void *in, size_t len);

///////////////////////////////////////////////////////////////////////////////
struct BDV_packet
{
   uint64_t bdvID_;
   BinaryData data_;

   BDV_packet(const uint64_t& id) :
      bdvID_(id)
   {}
};

///////////////////////////////////////////////////////////////////////////////
struct PendingMessage
{
   const uint64_t id_;
   const uint32_t msgid_;
   std::shared_ptr <::google::protobuf::Message> message_;

   PendingMessage(uint64_t id, uint32_t msgid, 
      std::shared_ptr<::google::protobuf::Message> msg) :
      id_(id), msgid_(msgid), message_(msg)
   {}
};

///////////////////////////////////////////////////////////////////////////////
struct ClientConnection
{
public:
   struct lws *wsiPtr_ = nullptr;

private:
   const uint64_t id_;
   BinaryData readLeftOverData_;

public:
   std::shared_ptr<Queue<SerializedMessage>> serializedStack_;
   mutable SerializedMessage currentWriteMsg_;
   std::shared_ptr<std::atomic<int>> count_;
   std::shared_ptr<BIP151Connection> bip151Connection_;
   std::shared_ptr<std::atomic<unsigned>> writeLock_, readLock_;
   std::chrono::time_point<std::chrono::system_clock> outKeyTimePoint_;
   std::shared_ptr<std::atomic<int>> run_;

   std::shared_ptr<Queue<BinaryData>> readQueue_;

private:
   void processAEADHandshake(BinaryData);

public:
   ClientConnection(struct lws *wsi, uint64_t id, AuthPeersLambdas& lbds) :
      wsiPtr_(wsi), id_(id)
   {
      bip151Connection_ = std::make_shared<BIP151Connection>(lbds);

      writeLock_ = std::make_shared<std::atomic<unsigned>>();
      writeLock_->store(0);

      readLock_ = std::make_shared<std::atomic<unsigned>>();
      readLock_->store(0);

      serializedStack_ = std::make_shared<Queue<SerializedMessage>>();
      readQueue_ = std::make_shared<Queue<BinaryData>>();
      
      count_ = std::make_shared<std::atomic<int>>();
      count_->store(0, std::memory_order_relaxed);

      run_ = std::make_shared<std::atomic<int>>();
      run_->store(0, std::memory_order_relaxed);
   }

   void closeConnection(void);
   void processReadQueue(std::shared_ptr<Clients>);
};

///////////////////////////////////////////////////////////////////////////////
class WebSocketServer
{
private:
   std::vector<std::thread> threads_;
   BlockingQueue<std::shared_ptr<BDV_packet>> packetQueue_;
   TransactionalMap<uint64_t, ClientConnection> clientStateMap_;

   static std::atomic<WebSocketServer*> instance_;
   static std::mutex mu_;
   static std::promise<bool> shutdownPromise_;
   static std::shared_future<bool> shutdownFuture_;
   BinaryData encInitPacket_;
   
   std::shared_ptr<Clients> clients_;
   std::atomic<unsigned> run_;
   std::promise<bool> isReadyProm_;

   BlockingQueue<std::unique_ptr<PendingMessage>> msgQueue_;
   BlockingQueue<uint64_t> clientConnectionInterruptQueue_;

   std::shared_ptr<AuthorizedPeers> authorizedPeers_;

private:
   void webSocketService(int port);
   void commandThread(void);
   void setIsReady(void);
   
   static WebSocketServer* getInstance(void);
   void prepareWriteThread(void);

   AuthPeersLambdas getAuthPeerLambda(void) const;
   void closeClientConnection(uint64_t);
   void clientInterruptThread(void);

public:
   WebSocketServer(void);

   static int callback(
      struct lws *wsi, enum lws_callback_reasons reason,
      void *user, void *in, size_t len);

   static void initAuthPeers(const PassphraseLambda&);
   static void start(BlockDataManagerThread* bdmT, bool async);
   static void shutdown(void);
   static void waitOnShutdown(void);
   static SecureBinaryData getPublicKey(void);

   static void write(const uint64_t&, const uint32_t&,
      std::shared_ptr<::google::protobuf::Message>);

   std::shared_ptr<const std::map<uint64_t, ClientConnection>>
      getConnectionStateMap(void) const;
   void addId(const uint64_t&, struct lws* ptr);
   void eraseId(const uint64_t&);
};

#endif
