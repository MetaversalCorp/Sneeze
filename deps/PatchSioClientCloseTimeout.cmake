# Idempotent patch for socket.io-client-cpp (sio_socket.cpp).
#
# sync_close() used to stall ~3s because socket::impl::close() arms a disconnect-ACK
# timer whose handler was socket::impl::on_close. That handler ignores error_code, so
# cancel()+reset() from another path queued on_close against a destroyed timer
# (use-after-free in asio::io_object_impl::get_service). Binding a timeout_close
# handler that returns on abort matches timeout_connection. Cancelling the timer in
# on_disconnect lets the io_service drain as soon as the WebSocket is gone.

if (NOT DEFINED SIO_SOCKET_CPP)
   message (FATAL_ERROR "SIO_SOCKET_CPP is not set")
endif ()
if (NOT EXISTS "${SIO_SOCKET_CPP}")
   message (FATAL_ERROR "SIO_SOCKET_CPP not found: ${SIO_SOCKET_CPP}")
endif ()

file (READ "${SIO_SOCKET_CPP}" _sio)
set (_changed FALSE)

if (NOT _sio MATCHES "void timeout_close\\(const asio::error_code")
   string (REPLACE
      "        void timeout_connection(const asio::error_code &ec);\n"
      "        void timeout_connection(const asio::error_code &ec);\n        void timeout_close(const asio::error_code &ec);\n"
      _sio "${_sio}")
   set (_changed TRUE)
endif ()

if (_sio MATCHES "async_wait\\(std::bind\\(&socket::impl::on_close, this\\)\\)")
   string (REPLACE
      "m_connection_timer->async_wait(std::bind(&socket::impl::on_close, this));"
      "m_connection_timer->async_wait(std::bind(&socket::impl::timeout_close, this, std::placeholders::_1));"
      _sio "${_sio}")
   set (_changed TRUE)
endif ()

if (NOT _sio MATCHES "void socket::impl::timeout_close")
   string (REPLACE
      "        this->on_close();\n    }\n    \n    void socket::impl::send_packet"
      "        this->on_close();\n    }\n\n    void socket::impl::timeout_close(const asio::error_code &ec)\n    {\n        NULL_GUARD(m_client);\n        if(ec)\n        {\n            return;\n        }\n        m_connection_timer.reset();\n        this->on_close();\n    }\n    \n    void socket::impl::send_packet"
      _sio "${_sio}")
   set (_changed TRUE)
endif ()

if (NOT _sio MATCHES "on_disconnect\\(\\)[^\n]*\n    \\{\n        NULL_GUARD\\(m_client\\);\n        if\\(m_connection_timer\\)")
   string (REPLACE
      "    void socket::impl::on_disconnect()\n    {\n        NULL_GUARD(m_client);\n        if(m_connected)\n        {"
      "    void socket::impl::on_disconnect()\n    {\n        NULL_GUARD(m_client);\n        if(m_connection_timer)\n        {\n            m_connection_timer->cancel();\n            m_connection_timer.reset();\n        }\n        if(m_connected)\n        {"
      _sio "${_sio}")
   set (_changed TRUE)
endif ()

if (_changed)
   file (WRITE "${SIO_SOCKET_CPP}" "${_sio}")
   message (STATUS "Patched ${SIO_SOCKET_CPP} (close-timeout)")
endif ()
