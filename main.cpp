#include <iostream>
#include "balancer.hpp"

using namespace std;
using namespace std::literals;
using namespace MgrSide;
using namespace ExtService;


template <typename T> concept STR = std::same_as<std::remove_cvref_t<T>, std::string>;



int main(int argc, char* argv[]) {
  std::string vm_addr {"172.16.116.129"};
  std::string loc_addr {"192.168.1.4"};
  std::string vm1_addr {"192.168.122.252"};
  std::string vm2_addr {"192.168.122.206"};
  std::string vm3_addr {"192.168.122.39"};
  std::string vm4_addr {"192.168.122.114"};
  std::string vm5_addr {"192.168.122.117"};

  int mode{};
  const uint16_t client_port{8801},  vm_port{8802}, push_port{8803};
  std::filesystem::path conf_path{"/home/fox/tmp/balancer/vm.json"};
  std::filesystem::path conn_path{"/home/fox/tmp/balancer/connection.json"};


  if (argc < 2) {
    return 0;
  }
  mode = stoi(argv[1]);
  if (mode == 1) {
    std::cout<<"Balance manager mode"<< std::endl<<std::flush;
    auto path{std::filesystem::current_path()};
    path /="vm.json";

    SrvMngr mgr{path, client_port, vm_port, push_port};
    for (auto count{0}; count <3; ++count) {
      mgr.mgrMonitor();
    }

  } else if (mode == 2) {
   std::cout<<"VM mode"<< std::endl<<std::flush;
   auto path{std::filesystem::current_path()};
   path /="conf.json";
   if (argc < 3) {
     std::cout<< " Sould be two parametrs - first run mode (1 - for server, 2 - for VM, 3 - for client)  second one is ip address of server";
     return 0;
   }
   std::string addr{argv[2]};
   VMServ vm_serv{addr, vm_port, push_port, path};
   //VMServ vm_serv{loc_addr, vm_port, push_port, path};

   int res;
   bool run = true;

   if (vm_serv.err.has_value()) {
     std::cout<<vm_serv.err.value();
   }

   while (run) {
     auto str = vm_serv.monitor();
     std::cout<<str<<std::endl<<std::flush;
     std::cin>>res;
     if (res) {
       run = false;
     }
   }

  } else if (mode == 3) {
    std::cout<<"Client mode"<< std::endl<<std::flush;
    std::string addr{argv[2]};
    Cln cln{addr, client_port};
    //Cln cln{loc_addr, client_port};

    if (argc < 3) {
      std::cout<< " Sould be two parametrs - first run mode (1 - for server, 2 - for VM, 3 - for client)  second one is ip address of server";
      return 0;
    }

    bool run{true};
    std::string ip_addr;
    while (run) {
      std::cout<<"Insert balancing addr"<< std::endl;
      std::cin>> ip_addr;
      if (ip_addr.size()<7) {
        run=false;
        continue;
        std::cout<<"Terminating"<< std::endl;
      }
      auto rec = cln.msg(ip_addr);
      if (rec.has_value()) {
        std::cout<<rec.value()<<std::endl;
      } else {
        std::cout<<rec.error()<<std::endl;
      }

    }
  }
  return 0;
}
