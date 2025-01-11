#ifndef BALANCER_HPP
#define BALANCER_HPP


#include <fstream>
#include <filesystem>
#include <concepts>
#include <limits>
#include <expected>
#include <optional>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>

#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <libvirt/libvirt.h>
#include "json.hpp"



/*  Communication formats:
 *  Server-client:
 * {   "New connection":
 *   {
 *   "ip":"172.172.0.201"
 *   }
 * }
 *  Server-VM:
 *  {   "Balance list":
 *   {
 *   "ipr1":["172.172.0.201", "172.172.0.202"],
 *   "ipr2":["173.72.0.201", "1723172.0.202"],
 *   }
 * }
 * PUSH servicde notification
 * { "Push":
 *   {
 *   "msg":"1"
 *   }
 * }
*/



namespace {
  template <typename T> concept FS = std::same_as<std::remove_cvref_t<T>, std::filesystem::path>;
  template <typename T> concept STR = std::same_as<std::remove_cvref_t<T>, std::string>;
  template <typename T> concept UI16 = std::same_as<std::remove_cvref_t<T>, uint16_t>;
  template <typename T> concept INT = std::same_as<std::remove_cvref_t<T>, int>;
  constexpr std::string LOOPBACK_INT {"127.0.0.1"};
  constexpr uint16_t MAX_DATA {1024};
}



// IP network interface
namespace NetSide {
  constexpr int SOCKET_ERROR{-1};
  constexpr int BACKLOG{10};
  constexpr int MSG_SIZE{512};


  // Storing requests to manager from VM and client
  class ReqStore final {
    public:
      std::atomic_bool redy_to_get{false};

      explicit ReqStore(std::atomic_bool* msg) : new_message{msg}{}

      template<STR T>
      [[nodiscard]] std::optional<std::string> store (T&& str) noexcept {
        std::optional<std::string> ret;
        std::lock_guard<std::mutex> store_mtx(mtx);
        try {
          msg_stor.emplace(std::forward<T>(str));
          *new_message=true;
          new_message->notify_all();
        } catch (const std::exception& e) {
          ret.emplace(e.what());
        }
        return ret;
      }
      [[nodiscard]] std::optional<std::string> getStore (void) noexcept {
        std::optional<std::string> ret;
        std::lock_guard<std::mutex> store_mtx(mtx);
        try {
          if (!msg_stor.empty()) {
            ret = msg_stor.front();
            msg_stor.pop();
          }
        } catch (...) {
          return {};
        }
        return ret;
      }
    private:
      std::mutex mtx;
      std::queue<std::string> msg_stor;
      std::atomic_bool* const new_message;

  };


  class BaseTCP {
    public:
      std::optional<std::string> err{};

      BaseTCP(uint16_t port) : port{port}{}
      virtual ~BaseTCP(){
        if(sock_fd) {
          close(sock_fd);
        }
        if(new_sock) {
          close(new_sock);
        }
      }
      BaseTCP(BaseTCP&) = delete;
      BaseTCP(BaseTCP&&) = delete;
      BaseTCP& operator = (BaseTCP&) = delete;
      BaseTCP& operator = (BaseTCP&&) = delete;

    protected:
      int sock_fd{0};
      int new_sock{0};
      uint16_t port{0};
     // THRQueue<int> sock_store;
      mutable std::atomic_bool new_message{false};

      void *get_in_addr(struct sockaddr *sa)
      {
          if (sa->sa_family == AF_INET) {
              return &(((struct sockaddr_in*)sa)->sin_addr);
          }

          return &(((struct sockaddr_in6*)sa)->sin6_addr);
      }

      template<STR T> [[nodiscrd]] std::expected<int64_t, std::string> snd_msg(T&& msg) const noexcept {
        std::expected<int64_t, std::string> ret;
        auto res = send(sock_fd, msg.c_str(), msg.size(), 0);
        if (res == SOCKET_ERROR) {
          return std::unexpected("Send operation error");
        } else {
          ret = res;
        }
        return ret;
      }
      template<STR T> [[nodiscrd]] std::expected<int64_t, std::string> new_snd_msg(T&& msg) noexcept {
        std::expected<int64_t, std::string> ret;

        auto res = send(new_sock, msg.c_str(), msg.size(), 0);
        if (res == SOCKET_ERROR) {
          return std::unexpected("Send operation error");
        } else {
          ret = res;
        }
        return ret;
      }
      [[nodiscrd]] std::optional<std::string> rcv_msg(void) noexcept  {
        std::optional<std::string> ret{};
        socklen_t sin_size;
        struct sockaddr_storage their_addr;
        char msg[MSG_SIZE];

        if (err.has_value()) {
          return ret;
        }

        sin_size = sizeof their_addr;
        new_sock = accept(sock_fd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_sock == SOCKET_ERROR) {
          err.emplace("Connection accept failed");
          return {};
        }
        auto res = recv(new_sock, msg, MSG_SIZE-1, 0);
        if (res == SOCKET_ERROR) {
          return ret;
        }
        return ret;
      }
  };

  class ClnTCP final : BaseTCP {
    public:
      using BaseTCP::err;
      using BaseTCP::snd_msg;
      using BaseTCP::rcv_msg;

      ClnTCP(const std::string& ip_addr, const uint16_t port) : BaseTCP(port) {
         struct addrinfo hints, *servinfo;
         std::string port_str{std::to_string(port)};
         int con_balancer;


         memset(&hints, 0, sizeof hints);
         hints.ai_family = AF_UNSPEC;
         hints.ai_socktype = SOCK_STREAM;
         if (auto rv {getaddrinfo(ip_addr.c_str(), port_str.c_str(), &hints, &servinfo)}; rv != 0) {
            err.emplace(gai_strerror(rv));
         }

         for(auto p {servinfo}; p != NULL; p = p->ai_next) {
           if (sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol); sock_fd == SOCKET_ERROR) {
             continue;
           }

          if (con_balancer = connect(sock_fd, p->ai_addr, p->ai_addrlen); con_balancer == SOCKET_ERROR) {
            close(sock_fd);
            continue;
          } else {
            break;
          }

       }
       freeaddrinfo(servinfo);
       if ((con_balancer == SOCKET_ERROR) && sock_fd) {
         close(sock_fd);
         sock_fd = 0;
       }
    }
    template<STR T> [[nodiscrd]] std::optional<std::string> snd_rcv_msg(T&& msg) const noexcept {
        std::optional<std::string> ret;
        char buf[MAX_DATA] = {0};
        if (err.has_value()) {
          return{};
        }
        auto res = send(sock_fd, msg.c_str(), msg.size(), 0);
        if (res == SOCKET_ERROR) {
          return {};
        }
        res = recv(sock_fd, buf, MAX_DATA-1, 0);
        if (res == SOCKET_ERROR) {
          return {};
        } else {
          ret = std::string{buf, static_cast<std::string::size_type>(res)};
        }
        return ret;
      }
  };

  class SrvTCP final: BaseTCP{
    public:
      using BaseTCP::err;
      using BaseTCP::rcv_msg;
      using BaseTCP::new_snd_msg;
      using BaseTCP::snd_msg;

      explicit  SrvTCP(const uint16_t port): BaseTCP(port) {
       init();
      }
      SrvTCP(const uint16_t port, std::shared_ptr<NetSide::ReqStore> store): BaseTCP(port), req_store {store} {
        init();
      }
      ~SrvTCP(){
        if(thr.joinable()) {
          s_source.request_stop();
          // TODO: make here client request to activatemsg listening process
          ClnTCP cln(LOOPBACK_INT, port);
          cln.snd_msg(std::string{" "});
          thr.join();
        }
      }
      SrvTCP(SrvTCP&) = delete;
      SrvTCP(SrvTCP&&) = delete;
      SrvTCP& operator = (SrvTCP&) = delete;
      SrvTCP& operator = (SrvTCP&&) = delete;

      std::optional<std::string> rec_addr(void) noexcept {
        std::optional<std::string> ret;
        struct sockaddr_storage their_addr;
        socklen_t sin_size;
        char msg[MSG_SIZE];

        if (err.has_value()) {
          return {};
        }
        new_sock = accept(sock_fd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_sock == SOCKET_ERROR) {
          err.emplace("Connection accept failed");
          return {};
        }
        auto res = recv(new_sock, msg, MSG_SIZE-1, 0);
        if (res == SOCKET_ERROR) {
          return {};
        } else {
          ret.emplace(std::string{msg, static_cast<std::string::size_type>(res)});
        }
        return ret;
      }

    private:
      void init (void) {
        struct addrinfo hints, *servinfo;
        int yes=1;
        std::string port_str{std::to_string(port)};

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        if (auto rv {getaddrinfo(NULL, port_str.c_str(), &hints, &servinfo)}; rv != 0) {
          err.emplace(gai_strerror(rv));
        }

        for(auto p {servinfo}; p != NULL; p = p->ai_next) {
          if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == SOCKET_ERROR) {
            continue;
         }

         if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == SOCKET_ERROR) {
           err.emplace("Socket options setup error");
           break;
         }

         if (bind(sock_fd, p->ai_addr, p->ai_addrlen) == SOCKET_ERROR) {
             close(sock_fd);
             continue;
         }
        break;
        }

        freeaddrinfo(servinfo); // all done with this structure
        if (listen(sock_fd, BACKLOG) == SOCKET_ERROR) {
          err.emplace("Listening setup error");
       }
      }
      std::thread thr;
      std::stop_source s_source;
      std::stop_token stoken;
      std::shared_ptr<NetSide::ReqStore> req_store;

      void rec_msg(const std::string conn_addr) noexcept {
        char msg[MSG_SIZE];
        if (err.has_value()) {
          return;
        }
        auto res = recv(new_sock, msg, MSG_SIZE-1, 0);
        if (res == SOCKET_ERROR) {
          return;
        } else {
          std::string str{msg, static_cast<size_t>(res)};
          nlohmann::json client_info;
          client_info["Client address"] = conn_addr.c_str();
          client_info["Client message"] = str.c_str();
          std::string stor_str = client_info.dump();
          if (req_store) {
            req_store->store(stor_str);
          }
          req_store->redy_to_get.notify_all();
        }
      }
  };
}

// VM Balancer manager
namespace MgrSide {
  constexpr char LibVirtDefaultPath[] {"qemu:///system"};

  // VM description
  struct VMData final {
    const std::string vmname;
    std::string oamip;
    std::string dataip;
    std::string mac;
  };

  using VMStore = std::unordered_map <std::string, VMData>;


  // Getting locval VM mac adresses
  class LibVirtIt {
    public:
      std::optional<std::string> err;
      char* xmlDesc = nullptr;

      explicit LibVirtIt(const char* vm_path = LibVirtDefaultPath) {
         conn = virConnectOpen(vm_path);
         if (!conn) {
           err.emplace("Connect to VM failed");
         }
      }
      template<STR T>
      explicit LibVirtIt(T&& vm_path) {
         conn = virConnectOpen(vm_path.c_str());
         if (!conn) {
           err.emplace("Connect to VM failed");
         }
      }

      virtual ~LibVirtIt() {
        if (conn) {
          virConnectClose(conn);
        }
      }

      LibVirtIt(LibVirtIt&) = delete;
      LibVirtIt(LibVirtIt&&) = delete;
      LibVirtIt& operator = (LibVirtIt&) = delete;
      LibVirtIt& operator = (LibVirtIt&&) = delete;

      template<STR T>
      [[nodiscard]] std::optional<std::string> getMAC(T&& vm_name) const noexcept {
        std::optional<std::string> ret;
        char* xmlDesc = nullptr;

        virDomainPtr domain = virDomainLookupByName(conn, vm_name.c_str());
        if (domain == nullptr) {
          return {};
         }
        if (xmlDesc =  virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_SECURE); !xmlDesc) {
           virDomainFree(domain);
           return {};
        }
        std::string xmlContent(xmlDesc);

        virDomainFree(domain);
        free(xmlDesc);

        size_t macPos = xmlContent.find("<mac address=");
        if (macPos != std::string::npos) {
           size_t startPos = xmlContent.find("=", macPos) + 2;
           size_t endPos = xmlContent.find("/>", startPos) - 1;
           std::string macAddress = xmlContent.substr(startPos, endPos - startPos);
           ret = macAddress;
         } else {
           return {};
         }
        return ret;
      }
    private :
      virConnectPtr conn{nullptr};
  };

  //  Creating balanced set (unordered_map) of source IP addresses
  class BalanceStore final {
    public:
      BalanceStore() {
        stor = std::make_unique<std::unordered_map<std::string, std::vector<std::string>>>();
      }

      template <STR T>
      [[nodiscard]] std::optional<std::string> setStore(T&& str_name, T&& str_msg) noexcept {
        std::optional<std::string> ret;
        const std::lock_guard<std::mutex> lock(mtx);

        try {
          if (auto stor_addr {stor->find(str_name)}; stor_addr != stor->end()){
            stor_addr->second.emplace_back(std::forward<T>(str_msg));
          } else {
            std::vector<std::string> vec;
            vec.emplace_back(std::forward<T>(str_msg));
            stor->emplace(std::make_pair(std::forward<T>(str_name), std::move(vec)));
          }
        } catch (...) {
          return {};
        }
        return ret;
      }
      [[nodiscard]] std::optional<std::string> getStore() noexcept {
        std::optional<std::string> ret;
        const std::lock_guard<std::mutex> lock(mtx);
        nlohmann::json ret_obj = nlohmann::json::object();

        try {
          for (const auto& [key, val] : *stor) {
            nlohmann::json arr = nlohmann::json::array();
            for(const auto &vec_it : val) {
              arr.push_back(vec_it);
            }
            ret_obj[key] = arr;
          }
          ret.emplace(nlohmann::to_string(ret_obj));
        } catch (...) {
          return {};
        }
        return ret;
      }
      size_t size(void) const noexcept {
        return stor->size();
      }
    private:
      std::mutex mtx;
      std::unique_ptr<std::unordered_map<std::string, std::vector<std::string>>> stor;
  };

  // Read basic vm configuration from a JSON config file
  class ReadConfJSON final : LibVirtIt {
    public:
      ReadConfJSON() : LibVirtIt() {};
      template<FS Fs, STR Str> ReadConfJSON(Fs&& path, Str&& vm_path) : LibVirtIt{std::forward<Str>(vm_path)} {
        init(std::forward<Fs>(path));
      }
      template<FS Fs> ReadConfJSON(Fs&& path) : LibVirtIt{} {
        init(std::forward<Fs>(path));
      }
      template<STR T> ReadConfJSON(T&& path, T&& vm_path) : LibVirtIt{vm_path} {
        if (path.size() <= PATH_MAX) {
          auto conf {std::filesystem::path(std::forward<T>(path))};
          init(std::move(conf));
        }
      }
      template<STR T> ReadConfJSON(T&& path) : LibVirtIt{} {
        if (path.size() <= PATH_MAX) {
          auto conf {std::filesystem::path(std::forward<T>(path))};
          init(std::move(conf));
        }
      }

      virtual ~ReadConfJSON() {
        if (conf_stream.is_open()) {
          conf_stream.close();
        }
      }
      ReadConfJSON& operator=(ReadConfJSON&) = delete;
      ReadConfJSON& operator=(ReadConfJSON&&) = delete;

      [[nodiscard]] std::optional<std::string> createStore(std::shared_ptr<VMStore> stor) const noexcept{
        std::optional<std::string> ret{};
        try{
          for (auto it{data.begin()}; it != data.end(); ++it) {
            std::string vmname {it->at("vmname")};
            std::string oamip {it->at("oamip")};
            std::string dataip {it->at("dataip")};
            std::string mac {it->at("mac")};
            VMData data{vmname, oamip, dataip, mac};
            if (auto macAddr{getMAC(vmname)}; macAddr.has_value()) {
              data.mac = macAddr.value();
            }
            stor->emplace(std::make_pair(vmname, data));
          }
        } catch (const std::exception& e) {
          ret = e.what();
        }

        return ret;
      }
    private :
      std::filesystem::path json_config{};
      std::ifstream conf_stream;
      nlohmann::json data;
      void init (const std::filesystem::path& path) noexcept{
        if (std::filesystem::exists(path)) {
          conf_stream.open(path);
          if (conf_stream.is_open()) {
             data = nlohmann::json::parse(conf_stream);
             conf_stream.close();
           }
         }
      }
      void init (std::filesystem::path&& path) noexcept {
        json_config = std::move(path);
        if (std::filesystem::exists(json_config)) {
          conf_stream.open(json_config);
          if (conf_stream.is_open()) {
             data = nlohmann::json::parse(conf_stream);
             conf_stream.close();
           }
         }
      }
  };



  // Creates interface for clients reqetst to update IP adress base and information on VM
  class SrvMngr final {
    public:
      std::optional<std::string> init_eror;

      template<FS T>
      SrvMngr(T&& conf_path, const uint16_t client_port, const uint16_t vm_port, const uint16_t push_port) : push_port {push_port} {

        //  Read config & create configuration base
        std::unique_ptr<ReadConfJSON> conf_maker = std::make_unique<ReadConfJSON>(std::forward<T>(conf_path));
        store = std::make_shared<VMStore>();
        init_eror = conf_maker->createStore(store);

        // Creating base for clients PUSH notifications &
        // making storage for balaced IP (divided by hash baskets as a ip adr vector)
        balance_stor = std::make_unique<BalanceStore>();

        //  Creating interfacees for VM & Client
        client_store = std::make_shared<NetSide::ReqStore>(&cln_req);
        vm_store = std::make_shared<NetSide::ReqStore>(&vm_req);
        client_int = std::make_unique<NetSide::SrvTCP>(client_port, client_store);
        vm_int = std::make_unique<NetSide::SrvTCP>(vm_port);

        thr_Client_process = std::thread(&SrvMngr::balsnceIP, this);
        if (!thr_Client_process.joinable()) {
          init_eror.emplace("Client process start failed");
        }
        thr_Client_process.detach();

        thr_VM_process = std::thread(&SrvMngr::processVMRequest, this);
        if (!thr_VM_process.joinable()) {
          init_eror.emplace("VM request processing start failed");
        }
        thr_VM_process.detach();

      }
      ~SrvMngr(){
        vm_work_run = false;
        cln_work_run = false;
      }
      SrvMngr(SrvMngr&) = delete;
      SrvMngr(SrvMngr&&) = delete;
      SrvMngr& operator = (SrvMngr&) = delete;
      SrvMngr& operator = (SrvMngr&&) = delete;
      void mgrStop(void) {
        vm_work_run = false;
        cln_work_run = false;
      }
      void mgrMonitor(void) noexcept {
        start_monitor.wait(false);
        start_monitor=false;
      }
    private :
      const uint16_t push_port;

      std::shared_ptr<VMStore> store;
      std::queue<std::string> order_set;
      std::unique_ptr<BalanceStore> balance_stor;

      std::unique_ptr<NetSide::SrvTCP> client_int, vm_int;
      std::shared_ptr<NetSide::ReqStore> client_store, vm_store;
      std::atomic_bool cln_req{false}, vm_req{false};
      std::atomic_bool cln_work_run{true}, vm_work_run{true};
      std::atomic_bool start_connect{false}, start_monitor{false};

      std::thread thr_VM_process, thr_Client_process;

      void balsnceIP(void) {
        std::string str_addr;
        while (vm_work_run) {
          auto msg = client_int->rec_addr();
          if (msg.has_value()) {
            std::string msg_value{msg.value()};
            auto json_msg = nlohmann::json::parse(msg_value);
            str_addr = json_msg.at("Balancing addres");
          } else {
            continue;
          }
          //  Staring balancing
          // Nothing to balance - first request cases
          if (auto balance_store_size {order_set.size()}; balance_store_size < store->size()) {
            auto store_it {std::begin(*store)};
            std::advance(store_it, balance_store_size);
            auto balance_addr{store_it->first};
            order_set.push(balance_addr);
            balance_stor->setStore(std::move(balance_addr), std::move(str_addr));
          } else { // Balancing existing pool
            if (order_set.size()) {
              auto balanced_name {order_set.front()};
              order_set.pop();
              order_set.push(balanced_name);
              balance_stor->setStore(std::move(balanced_name), std::move(str_addr));
            }
          }
          vm_push();
      }
    }
      // Sending update event notification to all VMs
      void vm_push(void) noexcept {
        nlohmann::json push_obj = nlohmann::json::parse(R"(
           { "Push":
             {
              "msg":"1"
             }
           }
        )");
        push_obj.dump();
        for (const auto& [key, val] : *store) {
          std::unique_ptr<NetSide::ClnTCP> push_vm = std::make_unique<NetSide::ClnTCP>(val.dataip, push_port);
          if (push_vm->err.has_value()) {
            continue;
          } else {
            auto rev = push_vm->snd_msg(nlohmann::to_string(push_obj));
          }
        }
      }
      //  VM balance list request processing
      void processVMRequest(void) noexcept {
        while (cln_work_run) {
          auto rec_data = vm_int->rcv_msg();
          if (!cln_work_run) {
            continue;
          }
          if (rec_data.has_value()) {
            continue;
          }

          auto balance_str{balance_stor->getStore()};
          if (balance_str.has_value()) {
            std::string str =  balance_str.value();
            auto snd_data = vm_int->new_snd_msg(str);
          }
          start_monitor = true;
          start_monitor.notify_all();
        }
      }
  };
}

// Communiacation interfases for VM and balancers client
namespace ExtService {
  // VM service
  class VMServ final {
    public:
      std::optional<std::string> err;
      template<STR Str, UI16 Ui16, FS Fs>
      VMServ (Str&& addr, Ui16&& vm_port, Ui16&& push_port, Fs&& path) :
        addr{std::forward<Str>(addr)}, vm_port {std::forward<Ui16>(vm_port)}, push_port{std::forward<Ui16>(push_port)}, path {std::forward<Fs>(path)} {
        fs_stream.open(path.string().c_str(), std::ios::trunc);
        if (!fs_stream.is_open()) {
          err.emplace("Balancing data file open error");
        }
        thr = std::thread(&VMServ::worker, this);
        if (!thr.joinable()) {
          err.emplace("VM notification service worker setup failed");
        }
        thr.detach();
      }
      ~VMServ() {
        const std::string str{" "};
        if (thr.joinable()) {
          stop_worker = true;
          auto snd_msg = std::make_unique<NetSide::ClnTCP>(LOOPBACK_INT, push_port);
          snd_msg->snd_msg(str);
        }
        fs_stream.close();
      }
      VMServ(VMServ&) = delete;
      VMServ(VMServ&&) = delete;
      VMServ& operator = (VMServ&) = delete;
      VMServ& operator = (VMServ&&) = delete;

      std::string monitor(void) {
        monitor_worker = false;
        monitor_worker.wait(false);
        return std::string {"Upd recieved"};
      }
    private :
      const std::string addr;
      const uint16_t vm_port, push_port;
      const std::filesystem::path path;
      std::ofstream fs_stream;

      std::thread thr;
      std::atomic_bool stop_worker{false};
      std::atomic_bool monitor_worker{false};

      void worker(void) noexcept {
        while (!stop_worker) {
          auto rec_push_msg = std::make_unique<NetSide::SrvTCP>(push_port);
          auto push_message = rec_push_msg->rcv_msg();
          if (stop_worker) {
            continue;
          }
          rec_push_msg.reset();
          auto snd_msg = std::make_unique<NetSide::ClnTCP>(addr, vm_port);
          auto new_balance_data = snd_msg->snd_rcv_msg(std::string{"port"});

          if (new_balance_data.has_value()) {
            std::string str{new_balance_data.value()};
            if (fs_stream.is_open()) {
              fs_stream.close();
            }
            fs_stream.open(path.string().c_str(), std::ios::trunc);
            fs_stream << new_balance_data.value()<<std::flush;
            monitor_worker = true;
            monitor_worker.notify_all();
          }

        }
      }
  };

  // Request for balance service to add new IP address
  class Cln final {
    public:
      Cln(const std::string& ip_addr, const uint16_t port) : ip_addr{ip_addr}, port{port}{}
      Cln(Cln&) = delete;
      Cln(Cln&&) = delete;
      Cln& operator = (Cln&) = delete;
      Cln& operator = (Cln&&) = delete;
      template<STR Str>
      [[nodiscard]] std::expected<int64_t, std::string> msg(Str&& srv_addr) const noexcept {
        NetSide::ClnTCP cln{ip_addr, port};
        nlohmann::json balancing_request;
        balancing_request["Balancing addres"] = srv_addr;
        auto snd = cln.snd_msg(nlohmann::to_string(balancing_request));
        return snd;
      }
    private :
      const std::string ip_addr;
      const uint16_t port;
  };
}
#endif // BALANCER_HPP
