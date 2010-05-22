//
// detail/reactive_socket_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2010 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_REACTIVE_SOCKET_SERVICE_HPP
#define ASIO_DETAIL_REACTIVE_SOCKET_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <boost/utility/addressof.hpp>
#include "asio/buffer.hpp"
#include "asio/error.hpp"
#include "asio/io_service.hpp"
#include "asio/socket_base.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/buffer_sequence_adapter.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/noncopyable.hpp"
#include "asio/detail/null_buffers_op.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/reactor_op.hpp"
#include "asio/detail/socket_accept_op.hpp"
#include "asio/detail/socket_connect_op.hpp"
#include "asio/detail/socket_holder.hpp"
#include "asio/detail/socket_ops.hpp"
#include "asio/detail/socket_recv_op.hpp"
#include "asio/detail/socket_recvfrom_op.hpp"
#include "asio/detail/socket_send_op.hpp"
#include "asio/detail/socket_sendto_op.hpp"
#include "asio/detail/socket_types.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename Protocol>
class reactive_socket_service
{
public:
  // The protocol type.
  typedef Protocol protocol_type;

  // The endpoint type.
  typedef typename Protocol::endpoint endpoint_type;

  // The native type of a socket.
  typedef socket_type native_type;

  // The implementation type of the socket.
  class implementation_type
    : private asio::detail::noncopyable
  {
  public:
    // Default constructor.
    implementation_type()
      : socket_(invalid_socket),
        flags_(0),
        protocol_(endpoint_type().protocol())
    {
    }

  private:
    // Only this service will have access to the internal values.
    friend class reactive_socket_service<Protocol>;

    // The native socket representation.
    socket_type socket_;

    enum
    {
      // The user wants a non-blocking socket.
      user_set_non_blocking = 1,

      // The implementation wants a non-blocking socket (in order to be able to
      // perform asynchronous read and write operations).
      internal_non_blocking = 2,

      // Helper "flag" used to determine whether the socket is non-blocking.
      non_blocking = user_set_non_blocking | internal_non_blocking,

      // User wants connection_aborted errors, which are disabled by default.
      enable_connection_aborted = 4,

      // The user set the linger option. Needs to be checked when closing.
      user_set_linger = 8
    };

    // Flags indicating the current state of the socket.
    unsigned char flags_;

    // The protocol associated with the socket.
    protocol_type protocol_;

    // Per-descriptor data used by the reactor.
    reactor::per_descriptor_data reactor_data_;
  };

  // Constructor.
  reactive_socket_service(asio::io_service& io_service)
    : io_service_impl_(use_service<io_service_impl>(io_service)),
      reactor_(use_service<reactor>(io_service))
  {
    reactor_.init_task();
  }

  // Destroy all user-defined handler objects owned by the service.
  void shutdown_service()
  {
  }

  // Construct a new socket implementation.
  void construct(implementation_type& impl)
  {
    impl.socket_ = invalid_socket;
    impl.flags_ = 0;
  }

  // Destroy a socket implementation.
  void destroy(implementation_type& impl)
  {
    if (impl.socket_ != invalid_socket)
    {
      reactor_.close_descriptor(impl.socket_, impl.reactor_data_);

      if (impl.flags_ & implementation_type::non_blocking)
      {
        ioctl_arg_type non_blocking = 0;
        asio::error_code ignored_ec;
        socket_ops::ioctl(impl.socket_, FIONBIO, &non_blocking, ignored_ec);
        impl.flags_ &= ~implementation_type::non_blocking;
      }

      if (impl.flags_ & implementation_type::user_set_linger)
      {
        ::linger opt;
        opt.l_onoff = 0;
        opt.l_linger = 0;
        asio::error_code ignored_ec;
        socket_ops::setsockopt(impl.socket_,
            SOL_SOCKET, SO_LINGER, &opt, sizeof(opt), ignored_ec);
      }

      asio::error_code ignored_ec;
      socket_ops::close(impl.socket_, ignored_ec);

      impl.socket_ = invalid_socket;
    }
  }

  // Open a new socket implementation.
  asio::error_code open(implementation_type& impl,
      const protocol_type& protocol, asio::error_code& ec)
  {
    if (is_open(impl))
    {
      ec = asio::error::already_open;
      return ec;
    }

    socket_holder sock(socket_ops::socket(protocol.family(),
          protocol.type(), protocol.protocol(), ec));
    if (sock.get() == invalid_socket)
      return ec;

    if (int err = reactor_.register_descriptor(sock.get(), impl.reactor_data_))
    {
      ec = asio::error_code(err,
          asio::error::get_system_category());
      return ec;
    }

    impl.socket_ = sock.release();
    impl.flags_ = 0;
    impl.protocol_ = protocol;
    ec = asio::error_code();
    return ec;
  }

  // Assign a native socket to a socket implementation.
  asio::error_code assign(implementation_type& impl,
      const protocol_type& protocol, const native_type& native_socket,
      asio::error_code& ec)
  {
    if (is_open(impl))
    {
      ec = asio::error::already_open;
      return ec;
    }

    if (int err = reactor_.register_descriptor(
          native_socket, impl.reactor_data_))
    {
      ec = asio::error_code(err,
          asio::error::get_system_category());
      return ec;
    }

    impl.socket_ = native_socket;
    impl.flags_ = 0;
    impl.protocol_ = protocol;
    ec = asio::error_code();
    return ec;
  }

  // Determine whether the socket is open.
  bool is_open(const implementation_type& impl) const
  {
    return impl.socket_ != invalid_socket;
  }

  // Destroy a socket implementation.
  asio::error_code close(implementation_type& impl,
      asio::error_code& ec)
  {
    if (is_open(impl))
    {
      reactor_.close_descriptor(impl.socket_, impl.reactor_data_);

      if (impl.flags_ & implementation_type::non_blocking)
      {
        ioctl_arg_type non_blocking = 0;
        asio::error_code ignored_ec;
        socket_ops::ioctl(impl.socket_, FIONBIO, &non_blocking, ignored_ec);
        impl.flags_ &= ~implementation_type::non_blocking;
      }

      if (socket_ops::close(impl.socket_, ec) == socket_error_retval)
        return ec;

      impl.socket_ = invalid_socket;
    }

    ec = asio::error_code();
    return ec;
  }

  // Get the native socket representation.
  native_type native(implementation_type& impl)
  {
    return impl.socket_;
  }

  // Cancel all operations associated with the socket.
  asio::error_code cancel(implementation_type& impl,
      asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    reactor_.cancel_ops(impl.socket_, impl.reactor_data_);
    ec = asio::error_code();
    return ec;
  }

  // Determine whether the socket is at the out-of-band data mark.
  bool at_mark(const implementation_type& impl,
      asio::error_code& ec) const
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return false;
    }

#if defined(SIOCATMARK)
    asio::detail::ioctl_arg_type value = 0;
    socket_ops::ioctl(impl.socket_, SIOCATMARK, &value, ec);
# if defined(ENOTTY)
    if (ec.value() == ENOTTY)
      ec = asio::error::not_socket;
# endif // defined(ENOTTY)
#else // defined(SIOCATMARK)
    int value = sockatmark(impl.socket_);
    if (value == -1)
      ec = asio::error_code(errno,
          asio::error::get_system_category());
    else
      ec = asio::error_code();
#endif // defined(SIOCATMARK)
    return ec ? false : value != 0;
  }

  // Determine the number of bytes available for reading.
  std::size_t available(const implementation_type& impl,
      asio::error_code& ec) const
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return 0;
    }

    asio::detail::ioctl_arg_type value = 0;
    socket_ops::ioctl(impl.socket_, FIONREAD, &value, ec);
#if defined(ENOTTY)
    if (ec.value() == ENOTTY)
      ec = asio::error::not_socket;
#endif // defined(ENOTTY)
    return ec ? static_cast<std::size_t>(0) : static_cast<std::size_t>(value);
  }

  // Bind the socket to the specified local endpoint.
  asio::error_code bind(implementation_type& impl,
      const endpoint_type& endpoint, asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    socket_ops::bind(impl.socket_, endpoint.data(), endpoint.size(), ec);
    return ec;
  }

  // Place the socket into the state where it will listen for new connections.
  asio::error_code listen(implementation_type& impl, int backlog,
      asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    socket_ops::listen(impl.socket_, backlog, ec);
    return ec;
  }

  // Set a socket option.
  template <typename Option>
  asio::error_code set_option(implementation_type& impl,
      const Option& option, asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    if (option.level(impl.protocol_) == custom_socket_option_level
        && option.name(impl.protocol_) == enable_connection_aborted_option)
    {
      if (option.size(impl.protocol_) != sizeof(int))
      {
        ec = asio::error::invalid_argument;
      }
      else
      {
        if (*reinterpret_cast<const int*>(option.data(impl.protocol_)))
          impl.flags_ |= implementation_type::enable_connection_aborted;
        else
          impl.flags_ &= ~implementation_type::enable_connection_aborted;
        ec = asio::error_code();
      }
      return ec;
    }
    else
    {
      if (option.level(impl.protocol_) == SOL_SOCKET
          && option.name(impl.protocol_) == SO_LINGER)
      {
        impl.flags_ |= implementation_type::user_set_linger;
      }

      socket_ops::setsockopt(impl.socket_,
          option.level(impl.protocol_), option.name(impl.protocol_),
          option.data(impl.protocol_), option.size(impl.protocol_), ec);

#if defined(__MACH__) && defined(__APPLE__) \
|| defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__)
      // To implement portable behaviour for SO_REUSEADDR with UDP sockets we
      // need to also set SO_REUSEPORT on BSD-based platforms.
      if (!ec && impl.protocol_.type() == SOCK_DGRAM
          && option.level(impl.protocol_) == SOL_SOCKET
          && option.name(impl.protocol_) == SO_REUSEADDR)
      {
        asio::error_code ignored_ec;
        socket_ops::setsockopt(impl.socket_, SOL_SOCKET, SO_REUSEPORT,
            option.data(impl.protocol_), option.size(impl.protocol_),
            ignored_ec);
      }
#endif

      return ec;
    }
  }

  // Set a socket option.
  template <typename Option>
  asio::error_code get_option(const implementation_type& impl,
      Option& option, asio::error_code& ec) const
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    if (option.level(impl.protocol_) == custom_socket_option_level
        && option.name(impl.protocol_) == enable_connection_aborted_option)
    {
      if (option.size(impl.protocol_) != sizeof(int))
      {
        ec = asio::error::invalid_argument;
      }
      else
      {
        int* target = reinterpret_cast<int*>(option.data(impl.protocol_));
        if (impl.flags_ & implementation_type::enable_connection_aborted)
          *target = 1;
        else
          *target = 0;
        option.resize(impl.protocol_, sizeof(int));
        ec = asio::error_code();
      }
      return ec;
    }
    else
    {
      size_t size = option.size(impl.protocol_);
      socket_ops::getsockopt(impl.socket_,
          option.level(impl.protocol_), option.name(impl.protocol_),
          option.data(impl.protocol_), &size, ec);
      if (!ec)
        option.resize(impl.protocol_, size);
      return ec;
    }
  }

  // Perform an IO control command on the socket.
  template <typename IO_Control_Command>
  asio::error_code io_control(implementation_type& impl,
      IO_Control_Command& command, asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    socket_ops::ioctl(impl.socket_, command.name(),
        static_cast<ioctl_arg_type*>(command.data()), ec);

    // When updating the non-blocking mode we always perform the ioctl
    // syscall, even if the flags would otherwise indicate that the socket is
    // already in the correct state. This ensures that the underlying socket
    // is put into the state that has been requested by the user. If the ioctl
    // syscall was successful then we need to update the flags to match.
    if (!ec && command.name() == static_cast<int>(FIONBIO))
    {
      if (*static_cast<ioctl_arg_type*>(command.data()))
      {
        impl.flags_ |= implementation_type::user_set_non_blocking;
      }
      else
      {
        // Clearing the non-blocking mode always overrides any internally-set
        // non-blocking flag. Any subsequent asynchronous operations will need
        // to re-enable non-blocking I/O.
        impl.flags_ &= ~(implementation_type::user_set_non_blocking
            | implementation_type::internal_non_blocking);
      }
    }

    return ec;
  }

  // Get the local endpoint.
  endpoint_type local_endpoint(const implementation_type& impl,
      asio::error_code& ec) const
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return endpoint_type();
    }

    endpoint_type endpoint;
    std::size_t addr_len = endpoint.capacity();
    if (socket_ops::getsockname(impl.socket_, endpoint.data(), &addr_len, ec))
      return endpoint_type();
    endpoint.resize(addr_len);
    return endpoint;
  }

  // Get the remote endpoint.
  endpoint_type remote_endpoint(const implementation_type& impl,
      asio::error_code& ec) const
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return endpoint_type();
    }

    endpoint_type endpoint;
    std::size_t addr_len = endpoint.capacity();
    if (socket_ops::getpeername(impl.socket_, endpoint.data(), &addr_len, ec))
      return endpoint_type();
    endpoint.resize(addr_len);
    return endpoint;
  }

  /// Disable sends or receives on the socket.
  asio::error_code shutdown(implementation_type& impl,
      socket_base::shutdown_type what, asio::error_code& ec)
  {
    socket_ops::shutdown(impl.socket_, what, ec);
    return ec;
  }

  // Send the given data to the peer.
  template <typename ConstBufferSequence>
  size_t send(implementation_type& impl, const ConstBufferSequence& buffers,
      socket_base::message_flags flags, asio::error_code& ec)
  {
    buffer_sequence_adapter<asio::const_buffer,
        ConstBufferSequence> bufs(buffers);

    return socket_ops::sync_send(impl.socket_, bufs.buffers(), bufs.count(),
        flags, bufs.all_empty(), impl.protocol_.type() == SOCK_STREAM,
        impl.flags_ & implementation_type::user_set_non_blocking, ec);
  }

  // Wait until data can be sent without blocking.
  size_t send(implementation_type& impl, const null_buffers&,
      socket_base::message_flags, asio::error_code& ec)
  {
    // Wait for socket to become ready.
    socket_ops::poll_write(impl.socket_, ec);

    return 0;
  }

  // Start an asynchronous send. The data being sent must be valid for the
  // lifetime of the asynchronous operation.
  template <typename ConstBufferSequence, typename Handler>
  void async_send(implementation_type& impl, const ConstBufferSequence& buffers,
      socket_base::message_flags flags, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef socket_send_op<ConstBufferSequence, Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(impl.socket_, buffers, flags, handler);

    start_op(impl, reactor::write_op, p.p, true,
        (impl.protocol_.type() == SOCK_STREAM
          && buffer_sequence_adapter<asio::const_buffer,
            ConstBufferSequence>::all_empty(buffers)));
    p.v = p.p = 0;
  }

  // Start an asynchronous wait until data can be sent without blocking.
  template <typename Handler>
  void async_send(implementation_type& impl, const null_buffers&,
      socket_base::message_flags, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef null_buffers_op<Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(handler);

    start_op(impl, reactor::write_op, p.p, false, false);
    p.v = p.p = 0;
  }

  // Send a datagram to the specified endpoint. Returns the number of bytes
  // sent.
  template <typename ConstBufferSequence>
  size_t send_to(implementation_type& impl, const ConstBufferSequence& buffers,
      const endpoint_type& destination, socket_base::message_flags flags,
      asio::error_code& ec)
  {
    buffer_sequence_adapter<asio::const_buffer,
        ConstBufferSequence> bufs(buffers);

    return socket_ops::sync_sendto(impl.socket_, bufs.buffers(), bufs.count(),
        flags, destination.data(), destination.size(),
        impl.flags_ & implementation_type::user_set_non_blocking, ec);
  }

  // Wait until data can be sent without blocking.
  size_t send_to(implementation_type& impl, const null_buffers&,
      socket_base::message_flags, const endpoint_type&,
      asio::error_code& ec)
  {
    // Wait for socket to become ready.
    socket_ops::poll_write(impl.socket_, ec);

    return 0;
  }

  // Start an asynchronous send. The data being sent must be valid for the
  // lifetime of the asynchronous operation.
  template <typename ConstBufferSequence, typename Handler>
  void async_send_to(implementation_type& impl,
      const ConstBufferSequence& buffers,
      const endpoint_type& destination, socket_base::message_flags flags,
      Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef socket_sendto_op<ConstBufferSequence,
        endpoint_type, Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(impl.socket_, buffers, destination, flags, handler);

    start_op(impl, reactor::write_op, p.p, true, false);
    p.v = p.p = 0;
  }

  // Start an asynchronous wait until data can be sent without blocking.
  template <typename Handler>
  void async_send_to(implementation_type& impl, const null_buffers&,
      socket_base::message_flags, const endpoint_type&, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef null_buffers_op<Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(handler);

    start_op(impl, reactor::write_op, p.p, false, false);
    p.v = p.p = 0;
  }

  // Receive some data from the peer. Returns the number of bytes received.
  template <typename MutableBufferSequence>
  size_t receive(implementation_type& impl,
      const MutableBufferSequence& buffers,
      socket_base::message_flags flags, asio::error_code& ec)
  {
    buffer_sequence_adapter<asio::mutable_buffer,
        MutableBufferSequence> bufs(buffers);

    return socket_ops::sync_recv(impl.socket_, bufs.buffers(), bufs.count(),
        flags, bufs.all_empty(), impl.protocol_.type() == SOCK_STREAM,
        impl.flags_ & implementation_type::user_set_non_blocking, ec);
  }

  // Wait until data can be received without blocking.
  size_t receive(implementation_type& impl, const null_buffers&,
      socket_base::message_flags, asio::error_code& ec)
  {
    // Wait for socket to become ready.
    socket_ops::poll_read(impl.socket_, ec);

    return 0;
  }

  // Start an asynchronous receive. The buffer for the data being received
  // must be valid for the lifetime of the asynchronous operation.
  template <typename MutableBufferSequence, typename Handler>
  void async_receive(implementation_type& impl,
      const MutableBufferSequence& buffers,
      socket_base::message_flags flags, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef socket_recv_op<MutableBufferSequence, Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    int protocol_type = impl.protocol_.type();
    p.p = new (p.v) op(impl.socket_, protocol_type, buffers, flags, handler);

    start_op(impl,
        (flags & socket_base::message_out_of_band)
          ? reactor::except_op : reactor::read_op,
        p.p, (flags & socket_base::message_out_of_band) == 0,
        (impl.protocol_.type() == SOCK_STREAM
          && buffer_sequence_adapter<asio::mutable_buffer,
            MutableBufferSequence>::all_empty(buffers)));
    p.v = p.p = 0;
  }

  // Wait until data can be received without blocking.
  template <typename Handler>
  void async_receive(implementation_type& impl, const null_buffers&,
      socket_base::message_flags flags, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef null_buffers_op<Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(handler);

    start_op(impl,
        (flags & socket_base::message_out_of_band)
          ? reactor::except_op : reactor::read_op,
        p.p, false, false);
    p.v = p.p = 0;
  }

  // Receive a datagram with the endpoint of the sender. Returns the number of
  // bytes received.
  template <typename MutableBufferSequence>
  size_t receive_from(implementation_type& impl,
      const MutableBufferSequence& buffers,
      endpoint_type& sender_endpoint, socket_base::message_flags flags,
      asio::error_code& ec)
  {
    buffer_sequence_adapter<asio::mutable_buffer,
        MutableBufferSequence> bufs(buffers);

    std::size_t addr_len = sender_endpoint.capacity();
    std::size_t bytes_recvd = socket_ops::sync_recvfrom(impl.socket_,
        bufs.buffers(), bufs.count(), flags, sender_endpoint.data(), &addr_len,
        impl.flags_ & implementation_type::user_set_non_blocking, ec);

    if (!ec)
      sender_endpoint.resize(addr_len);

    return bytes_recvd;
  }

  // Wait until data can be received without blocking.
  size_t receive_from(implementation_type& impl, const null_buffers&,
      endpoint_type& sender_endpoint, socket_base::message_flags,
      asio::error_code& ec)
  {
    // Wait for socket to become ready.
    socket_ops::poll_read(impl.socket_, ec);

    // Reset endpoint since it can be given no sensible value at this time.
    sender_endpoint = endpoint_type();

    return 0;
  }

  // Start an asynchronous receive. The buffer for the data being received and
  // the sender_endpoint object must both be valid for the lifetime of the
  // asynchronous operation.
  template <typename MutableBufferSequence, typename Handler>
  void async_receive_from(implementation_type& impl,
      const MutableBufferSequence& buffers, endpoint_type& sender_endpoint,
      socket_base::message_flags flags, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef socket_recvfrom_op<MutableBufferSequence,
        endpoint_type, Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    int protocol_type = impl.protocol_.type();
    p.p = new (p.v) op(impl.socket_, protocol_type,
        buffers, sender_endpoint, flags, handler);

    start_op(impl,
        (flags & socket_base::message_out_of_band)
          ? reactor::except_op : reactor::read_op,
        p.p, true, false);
    p.v = p.p = 0;
  }

  // Wait until data can be received without blocking.
  template <typename Handler>
  void async_receive_from(implementation_type& impl,
      const null_buffers&, endpoint_type& sender_endpoint,
      socket_base::message_flags flags, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef null_buffers_op<Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(handler);

    // Reset endpoint since it can be given no sensible value at this time.
    sender_endpoint = endpoint_type();

    start_op(impl,
        (flags & socket_base::message_out_of_band)
          ? reactor::except_op : reactor::read_op,
        p.p, false, false);
    p.v = p.p = 0;
  }

  // Accept a new connection.
  template <typename Socket>
  asio::error_code accept(implementation_type& impl,
      Socket& peer, endpoint_type* peer_endpoint, asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    // We cannot accept a socket that is already open.
    if (peer.is_open())
    {
      ec = asio::error::already_open;
      return ec;
    }

    // Accept a socket.
    for (;;)
    {
      // Try to complete the operation without blocking.
      socket_holder new_socket;
      std::size_t addr_len = 0;
      if (peer_endpoint)
      {
        addr_len = peer_endpoint->capacity();
        new_socket.reset(socket_ops::accept(impl.socket_,
              peer_endpoint->data(), &addr_len, ec));
      }
      else
      {
        new_socket.reset(socket_ops::accept(impl.socket_, 0, 0, ec));
      }

      // Check if operation succeeded.
      if (new_socket.get() >= 0)
      {
        if (peer_endpoint)
          peer_endpoint->resize(addr_len);
        peer.assign(impl.protocol_, new_socket.get(), ec);
        if (!ec)
          new_socket.release();
        return ec;
      }

      // Operation failed.
      if (ec == asio::error::would_block
          || ec == asio::error::try_again)
      {
        if (impl.flags_ & implementation_type::user_set_non_blocking)
          return ec;
        // Fall through to retry operation.
      }
      else if (ec == asio::error::connection_aborted)
      {
        if (impl.flags_ & implementation_type::enable_connection_aborted)
          return ec;
        // Fall through to retry operation.
      }
#if defined(EPROTO)
      else if (ec.value() == EPROTO)
      {
        if (impl.flags_ & implementation_type::enable_connection_aborted)
          return ec;
        // Fall through to retry operation.
      }
#endif // defined(EPROTO)
      else
        return ec;

      // Wait for socket to become ready.
      if (socket_ops::poll_read(impl.socket_, ec) < 0)
        return ec;
    }
  }

  // Start an asynchronous accept. The peer and peer_endpoint objects
  // must be valid until the accept's handler is invoked.
  template <typename Socket, typename Handler>
  void async_accept(implementation_type& impl, Socket& peer,
      endpoint_type* peer_endpoint, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef socket_accept_op<Socket, Protocol, Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    bool enable_connection_aborted =
      (impl.flags_ & implementation_type::enable_connection_aborted) != 0;
    p.p = new (p.v) op(impl.socket_, peer, impl.protocol_,
        peer_endpoint, enable_connection_aborted, handler);

    start_accept_op(impl, p.p, peer.is_open());
    p.v = p.p = 0;
  }

  // Connect the socket to the specified endpoint.
  asio::error_code connect(implementation_type& impl,
      const endpoint_type& peer_endpoint, asio::error_code& ec)
  {
    if (!is_open(impl))
    {
      ec = asio::error::bad_descriptor;
      return ec;
    }

    // Perform the connect operation.
    socket_ops::connect(impl.socket_,
        peer_endpoint.data(), peer_endpoint.size(), ec);
    if (ec != asio::error::in_progress
        && ec != asio::error::would_block)
    {
      // The connect operation finished immediately.
      return ec;
    }

    // Wait for socket to become ready.
    if (socket_ops::poll_connect(impl.socket_, ec) < 0)
      return ec;

    // Get the error code from the connect operation.
    int connect_error = 0;
    size_t connect_error_len = sizeof(connect_error);
    if (socket_ops::getsockopt(impl.socket_, SOL_SOCKET, SO_ERROR,
          &connect_error, &connect_error_len, ec) == socket_error_retval)
      return ec;

    // Return the result of the connect operation.
    ec = asio::error_code(connect_error,
        asio::error::get_system_category());
    return ec;
  }

  // Start an asynchronous connect.
  template <typename Handler>
  void async_connect(implementation_type& impl,
      const endpoint_type& peer_endpoint, Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef socket_connect_op<Handler> op;
    typename op::ptr p = { boost::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(impl.socket_, handler);

    start_connect_op(impl, p.p, peer_endpoint);
    p.v = p.p = 0;
  }

private:
  // Start the asynchronous read or write operation.
  void start_op(implementation_type& impl, int op_type,
      reactor_op* op, bool non_blocking, bool noop)
  {
    if (!noop)
    {
      if (is_open(impl))
      {
        if (!non_blocking || is_non_blocking(impl)
            || set_non_blocking(impl, op->ec_))
        {
          reactor_.start_op(op_type, impl.socket_,
              impl.reactor_data_, op, non_blocking);
          return;
        }
      }
      else
        op->ec_ = asio::error::bad_descriptor;
    }

    io_service_impl_.post_immediate_completion(op);
  }

  // Start the asynchronous accept operation.
  void start_accept_op(implementation_type& impl,
      reactor_op* op, bool peer_is_open)
  {
    if (!peer_is_open)
      start_op(impl, reactor::read_op, op, true, false);
    else
    {
      op->ec_ = asio::error::already_open;
      io_service_impl_.post_immediate_completion(op);
    }
  }

  // Start the asynchronous connect operation.
  void start_connect_op(implementation_type& impl,
      reactor_op* op, const endpoint_type& peer_endpoint)
  {
    if (is_open(impl))
    {
      if (is_non_blocking(impl) || set_non_blocking(impl, op->ec_))
      {
        if (socket_ops::connect(impl.socket_, peer_endpoint.data(),
              peer_endpoint.size(), op->ec_) != 0)
        {
          if (op->ec_ == asio::error::in_progress
              || op->ec_ == asio::error::would_block)
          {
            op->ec_ = asio::error_code();
            reactor_.start_op(reactor::connect_op,
                impl.socket_, impl.reactor_data_, op, false);
            return;
          }
        }
      }
    }
    else
      op->ec_ = asio::error::bad_descriptor;

    io_service_impl_.post_immediate_completion(op);
  }

  // Determine whether the socket has been set non-blocking.
  bool is_non_blocking(implementation_type& impl) const
  {
    return (impl.flags_ & implementation_type::non_blocking);
  }

  // Set the internal non-blocking flag.
  bool set_non_blocking(implementation_type& impl,
      asio::error_code& ec)
  {
    ioctl_arg_type non_blocking = 1;
    if (socket_ops::ioctl(impl.socket_, FIONBIO, &non_blocking, ec))
      return false;
    impl.flags_ |= implementation_type::internal_non_blocking;
    return true;
  }

  // The io_service implementation used to post completions.
  io_service_impl& io_service_impl_;

  // The selector that performs event demultiplexing for the service.
  reactor& reactor_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_REACTIVE_SOCKET_SERVICE_HPP
