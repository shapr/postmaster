#ifndef POSTMASTER_IO_HPP_2007_11_18
#define POSTMASTER_IO_HPP_2007_11_18

#include <boost/noncopyable.hpp>
#include <boost/function/function0.hpp>
#include <boost/function/function1.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_array.hpp>
#include <boost/compatibility/cpp_c_headers/cstddef>
#include <boost/compatibility/cpp_c_headers/ctime>
#include <boost/compatibility/cpp_c_headers/cerrno>
#include <boost/system/system_error.hpp>
#include <boost/bind.hpp>
#include <map>
#include <functional>
#include <vector>
#include <set>
#include <adns.h>
#include <poll.h>
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>

namespace postmaster
{
  // Basic system types for this platform.

  using std::size_t;
  using std::ptrdiff_t;
  using std::time_t;
  typedef unsigned int second_t;

  using boost::system::errno_ecat;
  using boost::system::system_error;

  // Throw system errors, but re-try on EINTR.

  template <class Result, class Cond, class Func>
  Result throw_errno_if(Cond failure, char const * ctx, Func f)
  {
    Result r;
    for (r = f(); failure(r); r = f())
    {
      if (errno != EINTR)
      {
        system_error error(errno, errno_ecat, ctx);
        throw error;
      }
    }
    return r;
  }

  template <class Func>
  int throw_errno_if_minus_one(char const * ctx, Func f)
  {
    return throw_errno_if<int>(boost::bind(std::equal_to<int>(), -1, _1), ctx, f);
  }

  namespace io
  {
    // Co-operative multi-tasking scheduler.

    class core : private boost::noncopyable
    {
    public:
      typedef boost::function0<void>                    task;

    private:
      typedef std::multimap<time_t,task>                task_queue;
      typedef std::pair<time_t,task_queue::iterator>    task_queue_entry;

    public:
      typedef task_queue_entry                          task_id;

      core() { update(); }

      task_id schedule(task const & t, second_t from_now = 0u)
      {
        BOOST_ASSERT(t);
        if (from_now > 0) update();
        task_queue::iterator const i( _tasks.insert(std::make_pair(_now + from_now, t)) );
        return std::make_pair(i->first, i);
      }

      time_t now() const { return _now; }

      void cancel(task_id & tid)
      {
        if (tid.first > _now)
        {
          _tasks.erase(tid.second);
          tid.first = 0;
        }
      }

      second_t run()
      {
        while (!_tasks.empty())
        {
          task_queue::iterator const i( _tasks.begin() );
          if (i->first > _now)
          {
            update();
            if (i->first > _now)
              return i->first - _now;
          }
          task t;
          t.swap(i->second);
          _tasks.erase(i);
          t();
        }
        return 0u;
      }

    protected:
      void update() { _now = std::time(0); }

    private:
      time_t            _now;
      task_queue        _tasks;
    };

    // I/O Event Dispatcher.

    class scheduler : public core
    {
    public:
      typedef int socket_id;

      explicit scheduler(unsigned int size_hint = 512u)
      {
        BOOST_ASSERT(size_hint <= static_cast<unsigned int>(std::numeric_limits<int>::max()));
        _epoll_fd = throw_errno_if_minus_one( "epoll_create(2) failed"
                                            , boost::bind(&epoll_create, static_cast<int>(size_hint))
                                            );
      }

      ~scheduler()
      {
        throw_errno_if_minus_one("epoll_create(2) failed", boost::bind(&close, _epoll_fd));
      }

      void run()
      {
        for (second_t next_event( core::run() ); !_handlers.empty(); next_event = core::run())
          run_epoll(next_event ? static_cast<int>(next_event) * 1000 : -1);
      }

      void register_socket(socket_id s)
      {
        std::cout << "register socket " << s << std::endl;
        BOOST_ASSERT(s >= 0);
        BOOST_ASSERT(_handlers.find(s) == _handlers.end());
        try
        {
          _handlers[s] = handler();
          epoll_event ev;
          ev.data.fd = s;
          ev.events  = 0;
          throw_errno_if_minus_one( "epoll_ctl() failed to add a socket"
                                  , boost::bind(&epoll_ctl, _epoll_fd, EPOLL_CTL_ADD, s, &ev)
                                  );
        }
        catch(...)
        {
          _handlers.erase(s);
          throw;
        }
      }

      void unregister_socket(socket_id s)
      {
        std::cout << "unregister socket " << s << std::endl;
        BOOST_ASSERT(s >= 0);
        handler_map::iterator const i( _handlers.find(s) );
        BOOST_ASSERT(i != _handlers.end());
        _handlers.erase(i);
        epoll_event ev;
        ev.data.fd = s;
        ev.events  = 0;
        throw_errno_if_minus_one( "epoll_ctl() failed to delete a socket"
                                , boost::bind(&epoll_ctl, _epoll_fd, EPOLL_CTL_DEL, s, &ev)
                                );
      }

      void on_input(socket_id s, task t)
      {
        BOOST_ASSERT(s >= 0);
        BOOST_ASSERT(_handlers.find(s) != _handlers.end());
        handler & h( _handlers[s] );
        h.first.swap(t);
        if ( static_cast<bool>(t) != static_cast<bool>(h.first) )
          modify_epoll(s, h, "scheduler::on_input() failed");
      }

      void on_output(socket_id s, task t)
      {
        BOOST_ASSERT(s >= 0);
        BOOST_ASSERT(_handlers.find(s) != _handlers.end());
        handler & h( _handlers[s] );
        h.second.swap(t);
        if ( static_cast<bool>(t) != static_cast<bool>(h.second) )
          modify_epoll(s, h, "scheduler::on_output() failed");
      }

    private:
      typedef std::pair<task,task>        handler;
      typedef std::map<socket_id,handler> handler_map;

      socket_id         _epoll_fd;
      handler_map       _handlers;

      void run_epoll(int timeout)
      {
        epoll_event ev[32u];
        int const rc( throw_errno_if_minus_one( "epoll_wait() failed"
                                              , boost::bind(&epoll_wait, _epoll_fd, ev, sizeof(ev) / sizeof(epoll_event), timeout)
                                              )
                    );
        update();
        for (int i(0); i != rc; ++i)
        {
          BOOST_ASSERT(ev[i].data.fd >= 0);
          BOOST_ASSERT(_handlers.find(ev[i].data.fd) != _handlers.end());
          handler & h( _handlers[ev[i].data.fd] );
          if ((ev[i].events & EPOLLIN)  && h.first)  schedule(h.first);
          if ((ev[i].events & EPOLLOUT) && h.second) schedule(h.second);
        }
      }

      void modify_epoll(socket_id s, handler & h, char const * ctxid)
      {
        epoll_event ev;
        ev.data.fd = s;
        ev.events  = (h.first ? EPOLLIN : 0) | (h.second ? EPOLLOUT : 0);
        throw_errno_if_minus_one(ctxid, boost::bind(&epoll_ctl, _epoll_fd, EPOLL_CTL_MOD, s, &ev));
      }
    };

    // Basic I/O interface.

    class system_socket;
    typedef boost::shared_ptr<system_socket> socket;

    class system_socket : private boost::noncopyable
    {
    public:
      typedef scheduler::task handler;

      ~system_socket()
      {
        _io.unregister_socket(_sock);
        if (_close_on_destruction)
          throw_errno_if_minus_one("close() failed", boost::bind(&close, _sock));
      }

      friend socket create_socket(scheduler & io, int fd)
      {
        socket s;
        s.reset( new system_socket(io, fd) );
        return s;
      }

      friend void on_input(socket s, handler f, second_t timeout = 0, handler h = handler())
      {
        s->_input.set<&scheduler::on_input>(s, f, timeout, h);
      }

      friend void on_output(socket s, handler f, second_t timeout = 0, handler h = handler())
      {
        s->_output.set<&scheduler::on_output>(s, f, timeout, h);
      }

      friend void close_on_destruction(socket s, bool b)
      {
        s->_close_on_destruction = b;
      }

    private:
      system_socket(scheduler & io, int fd) : _io(io), _sock(fd), _close_on_destruction(true)
      {
        BOOST_ASSERT(fd >= 0);
        _io.register_socket(_sock);
      }

      scheduler &               _io;
      int const                 _sock;
      bool                      _close_on_destruction;

      typedef void (scheduler::*registrar)(scheduler::socket_id, scheduler::task);

      class context : private boost::noncopyable
      {
      public:
        template <registrar on_event>
        void set(socket s, handler f, second_t to, handler h)
        {
          system_socket & sock( *s );
          sock._io.cancel(_timeout_id);
          _f.swap(f);
          _h.swap(h);
          _timeout = to;
          if (_f)
          {
            if (_timeout)
              _timeout_id = sock._io.schedule(boost::bind(&context::handle_timeout<on_event>, this, s), _timeout);
            (sock._io.*on_event)(sock._sock, boost::bind(&context::handle_event<on_event>, this, s));
          }
          else
            (sock._io.*on_event)(sock._sock, scheduler::task());
        }

      private:
        handler                 _f;
        handler                 _h;
        scheduler::task_id      _timeout_id;
        second_t                _timeout;

        template <registrar on_event>
        void handle_event(socket s)
        {
          system_socket & sock( *s );
          sock._io.cancel(_timeout_id);
          if (_f) _f();
          if (_f)
          {
            if (_timeout)
              _timeout_id = sock._io.schedule(boost::bind(&context::handle_timeout<on_event>, this, s), _timeout);
          }
          else
            (sock._io.*on_event)(sock._sock, scheduler::task());
        }

        template <registrar on_event>
        void handle_timeout(socket s)
        {
          handler h;
          h.swap(_h);
          set<on_event>(s, handler(), 0, handler());
          if (h) h();
        }
      };

      context _input, _output;
    };

    class resolver : public scheduler
    {
    public:
      typedef std::string                               hostname;
      typedef std::vector<hostname>                     hostname_list;

      typedef std::string                               hostaddr;
      typedef std::vector<hostaddr>                     hostaddr_list;

      typedef std::pair<hostname,hostaddr_list>         mxname;
      typedef std::vector<mxname>                       mxname_list;

      typedef boost::function1<void, hostaddr_list *>   a_handler;
      typedef boost::function1<void, mxname_list *>     mx_handler;
      typedef boost::function1<void, hostname *>        ptr_handler;

      resolver() : _scheduled(false)
      {
        adns_initflags const flags(static_cast<adns_initflags>( adns_if_debug
                                                              | adns_if_noautosys
                                                              | adns_if_nosigpipe
                                                              | adns_if_checkc_freq
                                                              ));
        throw_rc_if_not_zero("cannot initialize adns resolver", boost::bind(&adns_init, &_state, flags, static_cast<FILE*>(0)));
        BOOST_ASSERT(_state);
        update();
      }

      ~resolver()
      {
        release_fds();
        adns_finish(_state);
      }

      void query_a(char const * owner, a_handler const & h)
      {
        BOOST_ASSERT(h);
        submit(owner, adns_r_a, 0, boost::bind(handleA, _1, h));
      }

      void query_a_no_cname(char const * owner, a_handler const & h)
      {
        BOOST_ASSERT(h);
        submit(owner, adns_r_a, 0 | adns_qf_cname_forbid, boost::bind(handleA, _1, h));
      }

      void query_mx(char const * owner, mx_handler const & h)
      {
        BOOST_ASSERT(h);
        submit(owner, adns_r_mx, 0, boost::bind(handleMX, _1, h));
      }

      void query_ptr(char const * owner, ptr_handler const & h)
      {
        BOOST_ASSERT(h);
        submit(owner, adns_r_ptr, 0, boost::bind(handlePTR, _1, h));
      }

    private:
      typedef boost::shared_ptr<adns_answer const>      answer;
      typedef boost::function1<void, answer>            callback;
      typedef std::map<adns_query,callback>             query_set;

      adns_state        _state;
      query_set         _qset;
      bool              _scheduled;

      void schedule_deliver()
      {
        check_consistency();
        if (!_scheduled)
        {
          schedule(boost::bind(&resolver::deliver, this));
          _scheduled = true;
        }
      }

      void release_fds()
      {
        cancel(_timeout);
        fd_set fdset;
        fdset.swap(_registered_fds);
        std::for_each(fdset.begin(), fdset.end(), boost::bind(&scheduler::unregister_socket, this, _1));
      }

      void submit(char const * owner, adns_rrtype rrtype, int flags, callback f)
      {
        schedule_deliver();
        adns_query qid;
        throw_rc_if_not_zero( "adns_query() failed"
                            , boost::bind(&adns_submit, _state, owner, rrtype, static_cast<adns_queryflags>(flags), static_cast<FILE*>(0), &qid)
                            );
        _qset[qid].swap(f);
      }

      void register_fds()
      {
        std::cout << "re-register adns in scheduler" << std::endl;
        if (_qset.empty()) return release_fds();

        // Determine the file descriptors we have to probe for.

        boost::scoped_array<pollfd> fds( new pollfd[ADNS_POLLFDS_RECOMMENDED] );
        int nfds(ADNS_POLLFDS_RECOMMENDED);
        int timeout;
        update();
        for (int rc( ERANGE ); rc == ERANGE; /**/)
        {
          timeout = -1;
          rc = adns_beforepoll(_state, fds.get(), &nfds, &timeout, &_now);
          switch(rc)
          {
            case ERANGE:        BOOST_ASSERT(nfds > 0); fds.reset( new pollfd[nfds] ); break;
            case 0:             break;
            default:            throw system_error(rc, errno_ecat, "adns_beforepoll() failed");
          }
        }
        BOOST_ASSERT(nfds >= 0);

        // Re-register the descriptors in scheduler.

        fd_set registered_fds;
        for (int i(0); i != nfds; ++i)
        {
          BOOST_ASSERT(fds[i].fd >= 0);
          BOOST_ASSERT(fds[i].events & (POLLIN | POLLOUT));
          std::cout << "probe adns fd " << fds[i].fd << std::endl;
          registered_fds.insert(fds[i].fd);
        }
        std::vector<int> fdset;
        std::set_difference( _registered_fds.begin(), _registered_fds.end()
                           , registered_fds.begin(),  registered_fds.end()
                           , std::back_insert_iterator< std::vector<int> >(fdset)
                           );
        std::for_each(fdset.begin(), fdset.end(), boost::bind(&scheduler::unregister_socket, this, _1));
        fdset.resize(0);
        std::set_difference( registered_fds.begin(),  registered_fds.end()
                           , _registered_fds.begin(), _registered_fds.end()
                           , std::back_insert_iterator< std::vector<int> >(fdset)
                           );
        std::for_each(fdset.begin(), fdset.end(), boost::bind(&scheduler::register_socket, this, _1));
        _registered_fds.swap(registered_fds);
        for (int i(0); i != nfds; ++i)
        {
          if (fds[i].events & POLLIN)
            on_input(fds[i].fd, boost::bind(&resolver::process_fd, this, &adns_processreadable, fds[i].fd));
          else
            on_input(fds[i].fd, scheduler::task());
          if (fds[i].events & POLLOUT)
            on_output(fds[i].fd, boost::bind(&resolver::process_fd, this, &adns_processwriteable, fds[i].fd));
          else
            on_output(fds[i].fd, scheduler::task());
        }
        cancel(_timeout);
        timeout /= 1000;
        if (timeout == 0)  process_timeout();
        if (timeout > 0)   _timeout = schedule(boost::bind(&resolver::process_timeout, this), timeout);
      }

      typedef std::set<int>     fd_set;
      typedef fd_set::iterator  fd_set_iterator;

      fd_set            _registered_fds;
      timeval           _now;
      core::task_id     _timeout;

      void update()
      {
        _now.tv_sec  = now();
        _now.tv_usec = 0;
      }

      void process_fd(int (*f)(adns_state, int, timeval const *), int fd)
      {
        std::cout << "process adns fd " << fd << std::endl;
        schedule_deliver();
        update();
        throw_rc_if_not_zero("adns processing callback failed", boost::bind(f, _state, fd, &_now));
      }

      void process_timeout()
      {
        std::cout << "process adns timeouts" << std::endl;
        schedule_deliver();
        update();
        adns_processtimeouts(_state, &_now);
      }

      void deliver()
      {
        std::cout << "deliver adns events" << std::endl;
        _scheduled = false;
        check_consistency();
        answer ans;
        for (callback f; /**/; f.clear(), ans.reset())
        {
          adns_query      qid(0);
          adns_answer *   a(0);
          int const       rc( adns_check(_state, &qid, &a, 0) );
          switch (rc)
          {
            case ESRCH:   BOOST_ASSERT(_qset.empty());  return release_fds();
            case EAGAIN:  BOOST_ASSERT(!_qset.empty()); return register_fds();
            case 0:       break;
            default:      throw system_error(rc, errno_ecat, "adns_check() failed");
          }
          BOOST_ASSERT(a);
          ans.reset(a, &::free);
          query_set::iterator const i( _qset.find(qid) );
          BOOST_ASSERT(i != _qset.end());
          f.swap(i->second);
          _qset.erase(i);
          f(ans);
        }
        check_consistency();
      }

      void check_consistency() const
      {
#ifndef NDEBUG
        adns_forallqueries_begin(_state);
        for ( adns_query qid( adns_forallqueries_next(_state, 0) );
              qid != 0;
              qid = adns_forallqueries_next(_state, 0))
          BOOST_ASSERT(_qset.find(qid) != _qset.end());
#endif
      }

      static void handleA(answer a, a_handler h)
      {
        BOOST_ASSERT(a->type == adns_r_a);
        hostaddr_list hs;
        switch (a->status)
        {
          case adns_s_ok:
            BOOST_ASSERT(a->nrrs > 0);
            for (int i(0); i != a->nrrs; ++i)
            {
              char const * str( inet_ntoa(a->rrs.inaddr[i]) );
              BOOST_ASSERT(str);
              hs.push_back(str);
            }
            h(&hs);
            break;

          case adns_s_nxdomain:
          case adns_s_nodata:
            BOOST_ASSERT(a->nrrs == 0);
            h(&hs);
            break;

          default:
            BOOST_ASSERT(a->nrrs == 0);
            h(0);
            break;
        }
      }

      static void handleMX(answer a, mx_handler h)
      {
        BOOST_ASSERT(a->type == adns_r_mx);
        mxname_list mxs;
        std::multimap<int,adns_rr_hostaddr const *> mxmap;
        switch (a->status)
        {
          case adns_s_ok:
            BOOST_ASSERT(a->nrrs > 0);
            for (int i(0); i != a->nrrs; ++i)
            {
              adns_rr_inthostaddr const & addr( a->rrs.inthostaddr[i] );
              mxmap.insert(std::make_pair(addr.i, &addr.ha));
            }
            for (std::multimap<int,adns_rr_hostaddr const *>::iterator i(mxmap.begin()); i != mxmap.end(); ++i)
            {
              adns_rr_hostaddr const & addr( *i->second );
              hostaddr_list ha;
              if (addr.naddrs > 0)
                for (int j(0); j != addr.naddrs; ++j)
                  ha.push_back(inet_ntoa( addr.addrs[j].addr.inet.sin_addr ));
              mxs.push_back(std::make_pair(addr.host, ha));
            }
            h(&mxs);
            break;

          case adns_s_nxdomain:
          case adns_s_nodata:
            BOOST_ASSERT(a->nrrs == 0);
            h(&mxs);
            break;

          default:
            BOOST_ASSERT(a->nrrs == 0);
            h(0);
            break;
        }
      }

      static void handlePTR(answer a, ptr_handler h)
      {
        BOOST_ASSERT(a->type == adns_r_ptr);
        hostname ha;
        switch (a->status)
        {
          case adns_s_ok:
            BOOST_ASSERT(a->nrrs == 1);
            ha.assign(a->rrs.str[0]);
            h(&ha);
            break;

          case adns_s_nxdomain:
          case adns_s_nodata:
            BOOST_ASSERT(a->nrrs == 0);
            h(&ha);
            break;

          default:
            BOOST_ASSERT(a->nrrs == 0);
            h(0);
            break;
        }
      }

      template <class Func>
      static void throw_rc_if_not_zero(char const * ctx, Func f)
      {
        int const rc( f() );
        if (rc != 0) { system_error error(rc, errno_ecat, ctx); throw error; }
      }
    };
  }

  class system : public io::resolver
  {
  public:
    typedef int                                 exit_code;
    typedef boost::function<void (exit_code)>   exit_handler;
    typedef pid_t                               child_id;

    system() : _scheduled(false)
    {
      struct sigaction a;
      std::memset(&a, 0, sizeof(a));
      a.sa_handler = SIG_IGN;
      for (int i(0); i != 32; ++i) sigaction(i, &a, 0);
      a.sa_handler = &handle_signal;
      a.sa_flags = SA_NOCLDSTOP | SA_NODEFER;
      throw_errno_if_minus_one( "cannot install SIGCHLD handler"
                              , boost::bind(&sigaction, SIGCHLD, &a, static_cast<struct sigaction *>(0))
                              );
    }

    child_id execute(exit_handler const & f, char const * filepath, char const * const * argv)
    {
      return execute(f, filepath, argv, environ);
    }

    child_id execute(exit_handler const & f, char const * filepath, char const * const * argv, char const * const * envp)
    {
      BOOST_ASSERT(f);
      BOOST_ASSERT(filepath); BOOST_ASSERT(filepath[0]);
      BOOST_ASSERT(argv); BOOST_ASSERT(argv[0]);
      BOOST_ASSERT(envp);
      child_id const pid( throw_errno_if_minus_one("cannot fork() new process", boost::bind(&fork)) );
      if (pid == 0)             // child
      {
        execve(filepath, const_cast<char **>(argv), const_cast<char **>(envp));
        std::cerr << "execve(" << filepath << ") failed: " << system_error(errno, errno_ecat).what() << std::endl;
        _Exit(1);
      }
      _hmap[pid] = f;           // parent
      deliver();
      return pid;
    }

    void kill(child_id pid)
    {
      throw_errno_if_minus_one("kill(2) failed", boost::bind(&::kill, pid, SIGKILL));
      deliver();
    }

  private:
    typedef std::map<child_id,exit_handler>     handler_map;
    handler_map                                 _hmap;
    bool                                        _scheduled;

    static void handle_signal(int) { }

    void reschedule()
    {
      if (!_scheduled && !_hmap.empty())
      {
        schedule(boost::bind(&system::wait_for_children, this), 1u);
        _scheduled = true;
      }
    }

    void wait_for_children()
    {
      _scheduled = false;
      deliver();
    }

    void deliver()
    {
      for (;;)
      {
        int status;
        child_id const pid( throw_errno_if_minus_one("waitpid() failed", boost::bind(&waitpid, -1, &status, WNOHANG)) );
        if (pid == 0) return reschedule();
        handler_map::iterator const i( _hmap.find(pid) );
        if (i != _hmap.end())
        {
          exit_handler f;
          i->second.swap(f);
          _hmap.erase(i);
          f(status);
        }
      }
    }
  };

  void print(system::exit_code ec)
  {
    if (WIFEXITED(ec))            std::cout << "child process returned " << WEXITSTATUS(ec);
    else if (WIFSIGNALED(ec))     std::cout << "child process terminated by signal " << WTERMSIG(ec)
                                            << (WCOREDUMP(ec) ? " (core dumped)" : "");
    else                          std::cout << "child process returned unknown code " << ec;
    std::cout << std::endl;
  }

}

#endif // POSTMASTER_IO_HPP_2007_11_18