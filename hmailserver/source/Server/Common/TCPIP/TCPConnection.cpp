// Copyright (c) 2010 Martin Knafve / hMailServer.com.  
// http://www.hmailserver.com

#include "StdAfx.h"
#include "TCPConnection.h"

#include "DisconnectedException.h"

#include "../Util/ByteBuffer.h"
#include "../BO/TCPIPPorts.h"
#include "../Persistence/PersistentSecurityRange.h"

#include "LocalIPAddresses.h"
#include "IPAddress.h"
#include "IOOperation.h"
#include "CertificateVerifier.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
  

   TCPConnection::TCPConnection(ConnectionSecurity connection_security,
                                boost::asio::io_service& io_service, 
                                boost::asio::ssl::context& context,
                                shared_ptr<Event> disconnected,
                                AnsiString expected_remote_hostname) :
      connection_security_(connection_security),
      socket_(io_service),
      ssl_socket_(socket_, context),
      resolver_(io_service),
      timer_(io_service),
      receive_binary_(false),
      remote_port_(0),
      receive_buffer_(250000),
      disconnected_(disconnected),
      context_(context),
      is_ssl_(false),
      expected_remote_hostname_(expected_remote_hostname),
      is_client_(false),
      timeout_(0),
      connection_state_(StateDisconnected)
   {
      session_id_ = Application::Instance()->GetUniqueID();

      LOG_DEBUG("Creating session " + StringParser::IntToString(session_id_));
   
   }

   TCPConnection::~TCPConnection(void)
   {
      LOG_DEBUG("Ending session " + StringParser::IntToString(session_id_));

      if (disconnected_)
         disconnected_->Set();
   }

   bool
   TCPConnection::Connect(const AnsiString &remote_ip_address, long remotePort, const IPAddress &localAddress)
   {
#if _DEBUG
      if (!StringParser::IsValidIPAddress(remote_ip_address))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5506, "TCPConnection::Connect", 
            Formatter::Format("Attempting to connect to {0} - Not a valid IP address.", remote_ip_address));
      }
#endif

      remote_port_ = remotePort;
      remote_ip_address_ = remote_ip_address;
      is_client_ = true;

      LOG_TCPIP(Formatter::Format("Connecting to {0}:{1}...", remote_ip_address_, remotePort));

      if (!localAddress.IsAny())
      {
         boost::system::error_code error_code;

         if (localAddress.GetType() == IPAddress::IPV4)
            socket_.open(boost::asio::ip::tcp::v4(), error_code);
         else if (localAddress.GetType() == IPAddress::IPV6)
            socket_.open(boost::asio::ip::tcp::v6(), error_code);

         if (error_code)
         {
            String errorMessage = Formatter::Format("Failed to open local socket on IP address {0}", localAddress.ToString());
            ReportError(ErrorManager::Medium, 5520, "TCPConnection::Connect", errorMessage, error_code);
            return false;
         }

         socket_.bind(boost::asio::ip::tcp::endpoint(localAddress.GetAddress(), 0), error_code);


         if (error_code)
         {
            String errorMessage = Formatter::Format("Failed to bind to IP address {0}.", localAddress.ToString());
            ReportError(ErrorManager::Medium, 4330, "TCPConnection::Connect", errorMessage, error_code);

            boost::system::error_code ignored_error_code;
            socket_.close(ignored_error_code);
            return false;
         }
      }

      // Start an asynchronous resolve to translate the server and service names
      // into a list of endpoints.
      StartAsyncConnect_(remote_ip_address, remotePort);
      return true;
   }

   void
   TCPConnection::StartAsyncConnect_(const String &ip_adress, int port)
   {
      IPAddress adress;
      adress.TryParse(ip_adress, true);

      tcp::endpoint ep;
      ep.address(adress.GetAddress());
      ep.port(port);

      //// Check that we don't try to connect to a port we're listening on. Doing
      //// that could start evil loops.
      //tcp::endpoint endpoint = *endpoint_iterator;

      if (LocalIPAddresses::Instance()->IsLocalPort(ep.address(), remote_port_))
      {
         String sMessage; 
         sMessage.Format(_T("Could not connect to %s on port %d since this would mean connecting to myself."), remote_ip_address_, remote_port_);

         OnCouldNotConnect(sMessage);

         LOG_TCPIP(_T("TCPConnection - ") + sMessage);

         return;
      }

      // Attempt a connection to the first endpoint in the list. Each endpoint
      // will be tried until we successfully establish a connection.
      socket_.async_connect(ep,
         boost::bind(&TCPConnection::AsyncConnectCompleted, shared_from_this(), boost::asio::placeholders::error));
   }

   void
   TCPConnection::AsyncConnectCompleted(const boost::system::error_code& err)
   {
      if (err)
      {
         // Are there more addresses we should attempt to connect to?
         AnsiString error = err.message();
         OnCouldNotConnect("Host name: " + remote_ip_address_ + ", message: " + error);

         return;
      }

      Start();
   }

   void 
   TCPConnection::Start()
   {
      LOG_DEBUG(Formatter::Format("TCP connection started for session {0}", session_id_));

      connection_state_ = StateConnected;

      OnConnected();

      if (connection_security_ == CSSSL)
      {
         EnqueueHandshake();
      }
   }

   void
   TCPConnection::ProcessOperationQueue_(int recurse_level)
   {
      if (recurse_level > 10)
      {
         throw std::logic_error(Formatter::FormatAsAnsi("Recurse level {0} was reached in TCPConnection::ProcessOperationQueue_ for session {1}", recurse_level, session_id_));
      }

      // Pick out the next item to process...
      shared_ptr<IOOperation> operation = operation_queue_.Front();

      if (!operation)
      {
         // We're no longer sending...
         return;
      }

      switch (operation->GetType())
      {
      case IOOperation::BCTHandshake:
        {
           AsyncHandshake();
           break;
        }
      case IOOperation::BCTWrite:
         {
            shared_ptr<ByteBuffer> pBuf = operation->GetBuffer();
            AsyncWrite(pBuf);
            break;
         }
      case IOOperation::BCTRead:
         {
            AsyncRead(operation->GetString());               
            break;
         }
      case IOOperation::BCTShutdownSend:
         {
            Shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            operation_queue_.Pop(IOOperation::BCTShutdownSend);
            ProcessOperationQueue_(recurse_level + 1);
            break;
         }
      case IOOperation::BCTDisconnect:
         {
            Disconnect();
            operation_queue_.Pop(IOOperation::BCTDisconnect);
            ProcessOperationQueue_(recurse_level + 1);
            break;
         }

      }
   }
   

   void 
   TCPConnection::Shutdown(boost::asio::socket_base::shutdown_type what)
   {
      boost::system::error_code ignored_error;
      socket_.shutdown(what, ignored_error);
   }

   void 
   TCPConnection::EnqueueDisconnect()
   {
      ConnectionState current_connection_state = connection_state_;

      if (current_connection_state != StateConnected)
      {
         return;
      }

      connection_state_ = StatePendingDisconnect;

      shared_ptr<ByteBuffer> pBuf;
      shared_ptr<IOOperation> operation = shared_ptr<IOOperation>(new IOOperation(IOOperation::BCTDisconnect, pBuf));
      operation_queue_.Push(operation);

      ProcessOperationQueue_(0);
   }


   void 
   TCPConnection::Disconnect()
   {
      // Perform graceful shutdown. No more operations will be performed. 
      Shutdown(boost::asio::socket_base::shutdown_both);

      connection_state_ = StateDisconnected;
   }

   void 
   TCPConnection::ThrowIfNotConnected_()
   {

      ConnectionState current_connection_state = connection_state_;

      if (current_connection_state != StateConnected)
      {
         throw DisconnectedException();
      }
   }

   void 
   TCPConnection::EnqueueHandshake()
   {
      ThrowIfNotConnected_();

      shared_ptr<ByteBuffer> pBuf;
      shared_ptr<IOOperation> operation = shared_ptr<IOOperation>(new IOOperation(IOOperation::BCTHandshake, pBuf));
      operation_queue_.Push(operation);

      ProcessOperationQueue_(0);
   }


   void 
   TCPConnection::AsyncHandshake()
   {
      // To do peer verification, it must both be enabled globally and supported by the deriving class.
      bool enable_peer_verification = Configuration::Instance()->GetVerifyRemoteSslCertificate() && IsClient();
      
      int verify_mode = 0;

      boost::system::error_code error_code;

      if (enable_peer_verification)
      {
         verify_mode = boost::asio::ssl::context::verify_peer || boost::asio::ssl::context::verify_fail_if_no_peer_cert;
                
         if (!expected_remote_hostname_.IsEmpty())
         {
            ssl_socket_.set_verify_callback(CertificateVerifier(session_id_, connection_security_, expected_remote_hostname_), error_code);

            if (error_code.value() != 0)
            {
               String errorMessage = Formatter::Format(_T("Failed to set verification callback for host {0}"), expected_remote_hostname_);
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5510, "TCPConnection::AsyncHandshake", errorMessage, error_code);
               HandshakeFailed_(error_code);
               return;
            }
         }
      }
      else
      {
         verify_mode = boost::asio::ssl::context::verify_none;
      }

      ssl_socket_.set_verify_mode(verify_mode, error_code);
      if (error_code.value() != 0)
      {
         String error_message = Formatter::Format(_T("Failed to configure OpenSSL certificate verification: Mode: {0}"), verify_mode);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5144, "TCPConnection::AsyncHandshake", error_message, error_code);
         HandshakeFailed_(error_code);
         return;
      }


      if (expected_remote_hostname_.IsEmpty())
      {
         LOG_DEBUG(Formatter::Format("Performing SSL/TLS handshake for session {0}. Verify certificate: {1}", session_id_, enable_peer_verification));
      }
      else
      {
         LOG_DEBUG(Formatter::Format("Performing SSL/TLS handshake for session {0}. Verify certificate: {1}, Expected remote host name: {2}", session_id_, enable_peer_verification, expected_remote_hostname_));
      }

      boost::asio::ssl::stream_base::handshake_type handshakeType = IsClient() ?
         boost::asio::ssl::stream_base::client :
      boost::asio::ssl::stream_base::server;

      ssl_socket_.async_handshake(handshakeType,
         boost::bind(&TCPConnection::AsyncHandshakeCompleted, shared_from_this(),
         boost::asio::placeholders::error));
   }


   void 
   TCPConnection::AsyncHandshakeCompleted(const boost::system::error_code& error)
   {
      if (!error)
      {
         // Send welcome message to client.
         is_ssl_ = true;
         OnHandshakeCompleted();
      }
      else
      {
         HandshakeFailed_(error);
      }

      operation_queue_.Pop(IOOperation::BCTHandshake);
      ProcessOperationQueue_(0);

   }

   void
   TCPConnection::HandshakeFailed_(const boost::system::error_code& error)
   {
      // The SSL handshake failed. This may happen for example if the user who has connected
      // to the TCP/IP port disconnects immediately without sending any data.
      String sMessage;
      sMessage.Format(_T("TCPConnection - SSL handshake with client failed. Error code: %d, Message: %s, Remote IP: %s"), error.value(), String(error.message()), SafeGetIPAddress());
      LOG_TCPIP(sMessage);

      OnHandshakeFailed();
   }



   void 
   TCPConnection::EnqueueShutdownSend()
   {
      ThrowIfNotConnected_();

      shared_ptr<ByteBuffer> pBuf;
      shared_ptr<IOOperation> operation = shared_ptr<IOOperation>(new IOOperation(IOOperation::BCTShutdownSend, pBuf));
      operation_queue_.Push(operation);

      ProcessOperationQueue_(0);
   }

   void 
   TCPConnection::EnqueueRead()
   {
      EnqueueRead(GetCommandSeparator());
   }

   void 
   TCPConnection::EnqueueRead(const AnsiString &delimitor)
   {
      ThrowIfNotConnected_();

      shared_ptr<IOOperation> operation = shared_ptr<IOOperation>(new IOOperation(IOOperation::BCTRead, delimitor));
      operation_queue_.Push(operation);

      ProcessOperationQueue_(0);
   }

   void 
   TCPConnection::AsyncRead(const AnsiString &delimitor)
   {
      UpdateAutoLogoutTimer();

      function<void (const boost::system::error_code&, size_t)> AsyncReadCompletedFunction =
         boost::bind(&TCPConnection::AsyncReadCompleted, shared_from_this(),
         boost::asio::placeholders::error,
         boost::asio::placeholders::bytes_transferred);

      if (is_ssl_)
      {
         if (delimitor.GetLength() == 0)
            boost::asio::async_read(ssl_socket_, receive_buffer_, boost::asio::transfer_at_least(1), AsyncReadCompletedFunction);
         else
            boost::asio::async_read_until(ssl_socket_, receive_buffer_,  delimitor, AsyncReadCompletedFunction);
      }
      else
      {
         if (delimitor.GetLength() == 0)
         {
            boost::asio::async_read(socket_, receive_buffer_, boost::asio::transfer_at_least(1), AsyncReadCompletedFunction);
         }
         else
         {
            boost::asio::async_read_until(socket_, receive_buffer_, delimitor, AsyncReadCompletedFunction);
         }
      }

   }

   void 
   TCPConnection::AsyncReadCompleted(const boost::system::error_code& error,  size_t bytes_transferred)
   {
      UpdateAutoLogoutTimer();

      if (error.value() != 0)
      {
         if (connection_state_ != StateConnected)
         {
            // The read failed, but we've already started the disconnection. So we should not log the failure
            // or enqueue a new disconnect.
            return;
         }

         OnReadError(error.value());

         String message;
         message.Format(_T("The read operation failed. Bytes transferred: %d"), bytes_transferred);
         ReportDebugMessage(message, error);

         if (error.value() == boost::asio::error::not_found)
         {
            // read buffer is full...
            OnExcessiveDataReceived();
         }

         EnqueueDisconnect();
      }
      else
      {
         if (receive_binary_)
         {
            shared_ptr<ByteBuffer> pBuffer = shared_ptr<ByteBuffer>(new ByteBuffer());
            pBuffer->Allocate(receive_buffer_.size());

            std::istream is(&receive_buffer_);
            is.read((char*) pBuffer->GetBuffer(), receive_buffer_.size());

            try
            {
               ParseData(pBuffer);
            }
            catch (DisconnectedException&)
            {
               throw;
            }
            catch (...)
            {
               String message;
               message.Format(_T("An error occured while parsing data. Data size: %d"), pBuffer->GetSize());

               ReportError(ErrorManager::Medium, 5136, "TCPConnection::AsyncReadCompleted", message);

               throw;
            }
         }
         else
         {
            std::string s;
            std::istream is(&receive_buffer_);
            std::getline(is, s, '\r');

            // consume trailing \n on line.
            receive_buffer_.consume(1);

      #ifdef _DEBUG
            String sDebugOutput;
            sDebugOutput.Format(_T("RECEIVED: %s\r\n"), String(s));
            OutputDebugString(sDebugOutput);
      #endif

            try
            {
               ParseData(s);
            }
            catch (DisconnectedException&)
            {
               throw;
            }
            catch (...)
            {
               String message;
               message.Format(_T("An error occured while parsing data. Data length: %d, Data: %s."), s.size(), String(s));

               ReportError(ErrorManager::Medium, 5136, "TCPConnection::AsyncReadCompleted", message);

               throw;
            }
         }
      }

      operation_queue_.Pop(IOOperation::BCTRead);
      ProcessOperationQueue_(0);
   }

   void 
   TCPConnection::EnqueueWrite(const AnsiString &sData)
   {
      AnsiString sTemp = sData;
      char *pBuf = sTemp.GetBuffer();

      shared_ptr<ByteBuffer> pBuffer = shared_ptr<ByteBuffer>(new ByteBuffer());
      pBuffer->Add((BYTE*) pBuf, sData.GetLength());

#ifdef _DEBUG
      String sDebugOutput;
      sDebugOutput.Format(_T("SENT: %s"), String(sTemp));
      OutputDebugString(sDebugOutput);
#endif

      EnqueueWrite(pBuffer);

   }

   void 
   TCPConnection::EnqueueWrite(shared_ptr<ByteBuffer> pBuffer)
   {
      ThrowIfNotConnected_();

      shared_ptr<IOOperation> operation = shared_ptr<IOOperation>(new IOOperation(IOOperation::BCTWrite, pBuffer));

      operation_queue_.Push(operation);
      ProcessOperationQueue_(0);
   }

   void 
   TCPConnection::AsyncWrite(shared_ptr<ByteBuffer> buffer)
   {
      UpdateAutoLogoutTimer();

      function<void (const boost::system::error_code&, size_t)> AsyncWriteCompletedFunction =
         boost::bind(&TCPConnection::AsyncWriteCompleted, shared_from_this(),
         boost::asio::placeholders::error,
         boost::asio::placeholders::bytes_transferred);

      if (is_ssl_)
         boost::asio::async_write
         (ssl_socket_, boost::asio::buffer(buffer->GetCharBuffer(), buffer->GetSize()), AsyncWriteCompletedFunction);
      else
         boost::asio::async_write
         (socket_, boost::asio::buffer(buffer->GetCharBuffer(), buffer->GetSize()), AsyncWriteCompletedFunction);

      
   }

   void 
   TCPConnection::AsyncWriteCompleted(const boost::system::error_code& error,  size_t bytes_transferred)
   {
      UpdateAutoLogoutTimer();

      if (error.value() != 0)
      {
         if (connection_state_ != StateConnected)
         {
            // The write failed, but we've already started the disconnection. So we should not log the failure
            // or enqueue a new disconnect.
            return;
         }

         String message;
         message.Format(_T("The write operation failed. Bytes transferred: %d"), bytes_transferred);
         ReportDebugMessage(message, error);

         EnqueueDisconnect();
      }
      else
      {
         bool containsQueuedSendOperations = operation_queue_.ContainsQueuedSendOperation();

         if (!containsQueuedSendOperations)
         {
            OnDataSent();
         }
      }

      operation_queue_.Pop(IOOperation::BCTWrite);
      ProcessOperationQueue_(0);
   }

   IPAddress 
   TCPConnection::GetRemoteEndpointAddress()
   {
      boost::asio::ip::tcp::endpoint remoteEndpoint = socket_.remote_endpoint();
      return IPAddress(remoteEndpoint.address());
   }

   unsigned long 
   TCPConnection::GetLocalEndpointPort() 
   {
      boost::asio::ip::tcp::endpoint localEndpoint = socket_.local_endpoint();

      return localEndpoint.port();

   }

   void
   TCPConnection::SetReceiveBinary(bool binary)
   {
      receive_binary_ = binary;
   }

   String 
   TCPConnection::SafeGetIPAddress()
   {
      try
      {
         IPAddress address = socket_.remote_endpoint().address();

         return address.ToString();
      }
      catch (...)
      {
         return "";
      }
   }

   bool
   TCPConnection::IsClient()
   {
      return is_client_;
   }

   void  
   TCPConnection::SetSecurityRange(shared_ptr<SecurityRange> securityRange)
   {
      security_range_ = securityRange;
   }

   shared_ptr<SecurityRange>
   TCPConnection::GetSecurityRange()
   {
      if (!security_range_)
      {
         IPAddress address(socket_.remote_endpoint().address());

         security_range_ = PersistentSecurityRange::ReadMatchingIP(address);
      }

      return security_range_;
   }

   int
   TCPConnection::GetSessionID()
   {
      return session_id_;
   }


   AnsiString 
   TCPConnection::GetIPAddressString()
   {
      return GetRemoteEndpointAddress().ToString();
   }


   void 
   TCPConnection::SetTimeout(int seconds)
   {
      timeout_ = seconds;
   }


   void 
   TCPConnection::UpdateAutoLogoutTimer()
   {
      /*
         The timer instance is not thread safe for multiple concurrent callers.

         We may have multiple IO operations starting and stopping at the same time, so we need to synchronize
         lock to the timer from preventing multiple threads from accessing at the same time.
      */

      boost::mutex::scoped_lock lock(autologout_timer_);

      boost::system::error_code error_code;

      // Put a timeout...
      timer_.expires_from_now(boost::posix_time::seconds(timeout_), error_code);

      if (error_code)
      {
         ReportError(ErrorManager::Low, 5333, "TCPConnection::UpdateLogoutTimer", "An unknown error occurred while updating logout timer.", error_code);
         return;
      }

      timer_.async_wait(bind(&TCPConnection::OnTimeout, 
         boost::weak_ptr<TCPConnection>(shared_from_this()), _1));
   }

   void
   TCPConnection::OnTimeout(boost::weak_ptr<TCPConnection> connection, boost::system::error_code const& err)
   {
      boost::shared_ptr<TCPConnection> conn = connection.lock();
      if (!conn)
      {
         return;
      }

      if (err == asio::error::operation_aborted) 
      {
         // the timeout operation was cancelled.
         return;
      }

      String message;
      message.Format(_T("The client has timed out. Session: %d"), conn->GetSessionID());
      LOG_DEBUG(message);

      conn->Timeout();
   }

   void 
   TCPConnection::Timeout()
   {
      if (connection_state_ != StateConnected)
      {
         // We have already enqueued a disconnect. Since we're still alive, disconnect the socket.
         Disconnect();
         return;
      }

      // If we will attempt to send more data to the client, such as a timeout message, that message
      // will have 5 seconds before we give up.
      SetTimeout(5);

      // Let deriving clients send a disconnect message.
      OnConnectionTimeout();

      // Queue up a disconnection.
      EnqueueDisconnect();

      // Make sure the autologout timer is triggered. This is done in OnConnectionTimeout if we send
      // a timeout message, but if we don't we need to make sure its triggerd.
      UpdateAutoLogoutTimer();
   }

   void 
   TCPConnection::ReportError(ErrorManager::eSeverity sev, int code, const String &context, const String &message, const boost::system::system_error &error)
   {
      String formattedMessage;
      formattedMessage.Format(_T("%s Remote IP: %s, Error code: %d, Message: %s"), message, SafeGetIPAddress(), error.code().value(), String(error.what()));
      ErrorManager::Instance()->ReportError(sev, code, context, formattedMessage);         
   }

   void 
   TCPConnection::ReportError(ErrorManager::eSeverity sev, int code, const String &context, const String &message)
   {
      String formattedMessage;
      formattedMessage.Format(_T("%s Remote IP: %s"), message, SafeGetIPAddress());
      ErrorManager::Instance()->ReportError(sev, code, context, formattedMessage);         
   }
   void 
   TCPConnection::ReportDebugMessage(const String &message, const boost::system::error_code &error)
   {
      String formattedMessage;
      formattedMessage.Format(_T("%s Remote IP: %s, Session: %d, Code: %d, Message: %s"), message, SafeGetIPAddress(), GetSessionID(), error.value(), String(error.message()));
      LOG_DEBUG(formattedMessage);
   }

}
