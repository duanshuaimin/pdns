#include "config.h"
#ifdef HAVE_LUA
#include "ext/luawrapper/include/LuaContext.hpp"
#endif

#include <fstream>
#include <thread>
#include "namespaces.hh"
#include "logger.hh"
#include "rec-lua-conf.hh"
#include "sortlist.hh"
#include "filterpo.hh"
#include "syncres.hh"
#include "rpzloader.hh"
#include "base64.hh"
#include "remote_logger.hh"

GlobalStateHolder<LuaConfigItems> g_luaconfs; 

/* SO HOW DOES THIS WORK! AND PLEASE PAY ATTENTION!
   This function can be called at any time. It is expected to overwrite all the contents
   of LuaConfigItems, which is held in a GlobalStateHolder for RCU properties.

   This function can be called again at a later date, so you must make sure that anything you
   allow to be configured from here lives in g_luaconfs AND NOWHERE ELSE.

   If someone loads an empty Lua file, the default LuaConfigItems struct MUST MAKE SENSE.

   To make this easy on you, here is a LuaConfigItems constructor where you
   can set sane defaults:
*/

LuaConfigItems::LuaConfigItems()
{
  auto ds=std::unique_ptr<DSRecordContent>(dynamic_cast<DSRecordContent*>(DSRecordContent::make("19036 8 2 49aac11d7b6f6446702e54a1607371607a1a41855200fd2ce1cdde32f24e8fb5")));
  // this hurts physically
  dsAnchors[DNSName(".")] = *ds;
}

/* DID YOU READ THE STORY ABOVE? */

template <typename C>
typename C::value_type::second_type constGet(const C& c, const std::string& name)
{
  auto iter = c.find(name);
  if(iter == c.end())
    return 0;
  return iter->second;
}

#ifndef HAVE_LUA
void loadRecursorLuaConfig(const std::string& fname)
{
  if(!fname.empty())
    throw PDNSException("Asked to load a Lua configuration file '"+fname+"' in binary without Lua support");
}
#else


void loadRecursorLuaConfig(const std::string& fname)
{
  LuaConfigItems lci;

  LuaContext Lua;
  if(fname.empty())
    return;
  ifstream ifs(fname);
  if(!ifs) {
    theL()<<"Unable to read configuration file from '"<<fname<<"': "<<strerror(errno)<<endl;
    return;
  }
  Lua.writeFunction("clearSortlist", [&lci]() { lci.sortlist.clear(); });
  
  /* we can get: "1.2.3.4"
                 {"1.2.3.4", "4.5.6.7"}
		 {"1.2.3.4", {"4.5.6.7", "8.9.10.11"}}
  */

  map<string,DNSFilterEngine::PolicyKind> pmap{
    {"NoAction", DNSFilterEngine::PolicyKind::NoAction}, 
    {"Drop", DNSFilterEngine::PolicyKind::Drop},
    {"NXDOMAIN", DNSFilterEngine::PolicyKind::NXDOMAIN},
    {"NODATA", DNSFilterEngine::PolicyKind::NODATA},
    {"Truncate", DNSFilterEngine::PolicyKind::Truncate},
    {"Custom", DNSFilterEngine::PolicyKind::Custom}
  };
  Lua.writeVariable("Policy", pmap);

  Lua.writeFunction("rpzFile", [&lci](const string& fname, const boost::optional<std::unordered_map<string,boost::variant<int, string>>>& options) {
      try {
	boost::optional<DNSFilterEngine::Policy> defpol;
	std::string polName;
	if(options) {
	  auto& have = *options;
	  if(have.count("policyName")) {
	    polName = boost::get<std::string>(constGet(have, "policyName"));
	  }
	  if(have.count("defpol")) {
	    defpol=DNSFilterEngine::Policy();
	    defpol->d_kind = (DNSFilterEngine::PolicyKind)boost::get<int>(constGet(have, "defpol"));
	    defpol->d_name = polName;
	    if(defpol->d_kind == DNSFilterEngine::PolicyKind::Custom) {
	      defpol->d_custom=
		shared_ptr<DNSRecordContent>(
					     DNSRecordContent::mastermake(QType::CNAME, 1, 
									  boost::get<string>(constGet(have,"defcontent"))
									  )
					     );
	 
	      if(have.count("defttl"))
		defpol->d_ttl = boost::get<int>(constGet(have, "defttl"));
	      else
		defpol->d_ttl = -1; // get it from the zone
	    }
	  }
	}
        theL()<<Logger::Warning<<"Loading RPZ from file '"<<fname<<"'"<<endl;
	loadRPZFromFile(fname, lci.dfe, polName, defpol, 0);
        theL()<<Logger::Warning<<"Done loading RPZ from file '"<<fname<<"'"<<endl;
      }
      catch(std::exception& e) {
	theL()<<Logger::Error<<"Unable to load RPZ zone from '"<<fname<<"': "<<e.what()<<endl;
      }
    });


  Lua.writeFunction("rpzMaster", [&lci](const string& master_, const string& zone_, const boost::optional<std::unordered_map<string,boost::variant<int, string>>>& options) {
      try {
	boost::optional<DNSFilterEngine::Policy> defpol;
        TSIGTriplet tt;
        int refresh=0;
	std::string polName;
	if(options) {
	  auto& have = *options;
	  if(have.count("policyName")) {
	    polName = boost::get<std::string>(constGet(have, "policyName"));
	  }
	  if(have.count("defpol")) {
	    //	    cout<<"Set a default policy"<<endl;
	    defpol=DNSFilterEngine::Policy();
	    defpol->d_kind = (DNSFilterEngine::PolicyKind)boost::get<int>(constGet(have, "defpol"));
	    defpol->d_name = polName;
	    if(defpol->d_kind == DNSFilterEngine::PolicyKind::Custom) {
	      //	      cout<<"Setting a custom field even!"<<endl;
	      defpol->d_custom=
		shared_ptr<DNSRecordContent>(
					     DNSRecordContent::mastermake(QType::CNAME, 1, 
									  boost::get<string>(constGet(have,"defcontent"))
									  )
					     );
	      if(have.count("defttl"))
		defpol->d_ttl = boost::get<int>(constGet(have, "defttl"));
	      else
		defpol->d_ttl = -1; // get it from the zone
	    }
	  }
	  if(have.count("tsigname")) {
            tt.name=DNSName(toLower(boost::get<string>(constGet(have, "tsigname"))));
            tt.algo=DNSName(toLower(boost::get<string>(constGet(have, "tsigalgo"))));
            if(B64Decode(boost::get<string>(constGet(have, "tsigsecret")), tt.secret))
              throw std::runtime_error("TSIG secret is not valid Base-64 encoded");
          }
          if(have.count("refresh")) {
            refresh = boost::get<int>(constGet(have,"refresh"));
          }
	}
	ComboAddress master(master_, 53);
	DNSName zone(zone_);

	auto sr=loadRPZFromServer(master, zone, lci.dfe, polName, defpol, 0, tt);
        if(refresh)
          sr->d_st.refresh=refresh;
	std::thread t(RPZIXFRTracker, master, zone, polName, tt, sr);
	t.detach();
      }
      catch(std::exception& e) {
	theL()<<Logger::Error<<"Unable to load RPZ zone '"<<zone_<<"' from '"<<master_<<"': "<<e.what()<<endl;
      }
      catch(PDNSException& e) {
	theL()<<Logger::Error<<"Unable to load RPZ zone '"<<zone_<<"' from '"<<master_<<"': "<<e.reason<<endl;
      }

    });

  typedef vector<pair<int,boost::variant<string, vector<pair<int, string> > > > > argvec_t;
  Lua.writeFunction("addSortList", 
		    [&lci](const std::string& formask_, 
		       const boost::variant<string, argvec_t>& masks,
		       boost::optional<int> order_) 
		    {
		      try {
			Netmask formask(formask_);
			int order = order_ ? (*order_) : lci.sortlist.getMaxOrder(formask)+1;
			if(auto str = boost::get<string>(&masks)) 
			  lci.sortlist.addEntry(formask, Netmask(*str), order);
			else {
	
			  auto vec = boost::get<argvec_t>(&masks);
			  for(const auto& e : *vec) {
			    if(auto s = boost::get<string>(&e.second)) {
			      lci.sortlist.addEntry(formask, Netmask(*s), order);
			    }
			    else {
			      const auto& v =boost::get<vector<pair<int, string> > >(e.second);
			      for(const auto& e : v)
				lci.sortlist.addEntry(formask, Netmask(e.second), order);
			    }
			    ++order;
			  }
			}
		      }
		      catch(std::exception& e) {
			theL()<<Logger::Error<<"Error in addSortList: "<<e.what()<<endl;
		      }
		    });

  Lua.writeFunction("addDS", [&lci](const std::string& who, const std::string& what) {
      lci.dsAnchors[DNSName(who)]= *std::unique_ptr<DSRecordContent>(dynamic_cast<DSRecordContent*>(DSRecordContent::make(what)));
    });

  Lua.writeFunction("clearDS", [&lci](boost::optional<string> who) {
      if(who)
        lci.dsAnchors.erase(DNSName(*who));
      else
        lci.dsAnchors.clear();
    });

#if HAVE_PROTOBUF
  Lua.writeFunction("protobufServer", [&lci](const string& server_, const boost::optional<uint16_t> timeout, const boost::optional<uint64_t> maxQueuedEntries, const boost::optional<uint8_t> reconnectWaitTime) {
      try {
	ComboAddress server(server_);
        if (!lci.protobufServer) {
          lci.protobufServer = std::make_shared<RemoteLogger>(server, timeout ? *timeout : 2, maxQueuedEntries ? *maxQueuedEntries : 100, reconnectWaitTime ? *reconnectWaitTime : 1);
        }
        else {
          theL()<<Logger::Error<<"Only one protobuf server can be configured, we already have "<<lci.protobufServer->toString()<<endl;
        }
      }
      catch(std::exception& e) {
	theL()<<Logger::Error<<"Error while starting protobuf logger to '"<<server_<<": "<<e.what()<<endl;
      }
      catch(PDNSException& e) {
        theL()<<Logger::Error<<"Error while starting protobuf logger to '"<<server_<<": "<<e.reason<<endl;
      }
    });
#endif

  try {
    Lua.executeCode(ifs);
    g_luaconfs.setState(lci);
  }
  catch(const LuaContext::ExecutionErrorException& e) {
    theL()<<Logger::Error<<"Unable to load Lua script from '"+fname+"': ";
    try {
      std::rethrow_if_nested(e);
    } catch(const std::exception& e) {
      // e is the exception that was thrown from inside the lambda
      theL() << e.what() << std::endl;      
    }
    catch(const PDNSException& e) {
      // e is the exception that was thrown from inside the lambda
      theL() << e.reason << std::endl;      
    }
    throw;

  }
  catch(std::exception& err) {
    theL()<<Logger::Error<<"Unable to load Lua script from '"+fname+"': "<<err.what()<<endl;
    throw;
  }

}

#endif
