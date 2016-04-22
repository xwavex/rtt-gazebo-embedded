#include <rtt_gazebo_embedded/rtt_gazebo_embedded.hh>

using namespace RTT;
using namespace RTT::os;
using namespace std;

RTTGazeboEmbedded::RTTGazeboEmbedded(const std::string& name):
TaskContext(name),
world_path("worlds/empty.world"),
model_name(name),
go_sem(0),
model_timeout_s(20.0),
use_rtt_sync(false)
//is_model_configured(false)
{
    RTT::log(RTT::Info) << "Creating " << name <<" with gazebo embedded !" << RTT::endlog();
    this->ports()->addPort("sync",port_sync).doc("Migth be used to trigger your component's updateHook().");
    this->addProperty("use_rtt_sync",use_rtt_sync).doc("Gazebo tries to run at the component's rate (or slower).");
    this->addProperty("world_path",world_path).doc("The path to the .world file.");
    this->addOperation("add_plugin",&RTTGazeboEmbedded::addPlugin,this,RTT::OwnThread).doc("The path to a plugin file.");
    this->addOperation("getWorldPtr",&RTTGazeboEmbedded::getWorldPtr,this,RTT::OwnThread).doc("The world to get models from.");
    this->addOperation("getModelPtr",&RTTGazeboEmbedded::getModelPtr,this,RTT::ClientThread).doc("Get the model directly");
    this->addProperty("argv",argv).doc("argv passed to the deployer's main.");
//         this->addProperty("model_name",model_name).doc("The name of the robot.");
//         this->addProperty("model_timeout_s",model_timeout_s).doc("Time during which we wait for the model to be spawned.");
    this->addProperty("run_world_elapsed",run_world_elapsed).doc("Duration of run world");
//         this->addOperation("isModelConfigured",&RTTGazeboEmbedded::isModelConfigured,this,RTT::ClientThread).doc("True if the model has been loaded.");

//         this->ports()->addPort("JointPosition", port_joint_position_out).doc("");
//         this->ports()->addPort("JointVelocity", port_joint_velocity_out).doc("");
//         this->ports()->addPort("JointTorque", port_joint_torque_out).doc("");
// 
//         this->ports()->addPort("JointPositionCommand", port_joint_position_cmd_in).doc("");
//         this->ports()->addPort("JointVelocityCommand", port_joint_velocity_cmd_in).doc("");
//         this->ports()->addPort("JointTorqueCommand", port_joint_torque_cmd_in).doc("");

    this->addOperation("readyROSService",&RTTGazeboEmbedded::readyROSService,this,RTT::OwnThread).doc("Say everyone the component is ready.");

    gazebo::printVersion();
    gazebo::common::Console::SetQuiet(false);
}
gazebo::physics::ModelPtr RTTGazeboEmbedded::getModelPtr(const std::string& model_name,double timeout_s)
{
    RTT::log(RTT::Info) <<"["<<getName()<<"] Trying to get "<<model_name<<" in less then "<<timeout_s<<"s" << RTT::endlog();
    gazebo::physics::ModelPtr model = nullptr;
    auto tstart = TimeService::Instance()->getTicks();
    while(true)
    {
        auto elapsed = TimeService::Instance()->getSeconds(tstart);
        if(elapsed > timeout_s) break;

        // Exit gazebo run loop
        gazebo::runWorld(world, 1);

        model = world->GetModel(model_name);

        if(model){
            RTT::log(RTT::Info) << "["<<getName()<<"] Model ["<<model_name<<"] acquired !" << RTT::endlog();
            run_th.join();
            this->startHook();
            return model;
        }

        usleep(1E6);
        RTT::log(RTT::Info) << "["<<getName()<<"] waiting for model ["<<model_name<<"] to come up" << RTT::endlog();
    }
    
    RTT::log(RTT::Error) << "["<<getName()<<"] Model ["<<model_name<<"] timed out" << RTT::endlog();
    return nullptr;
}

gazebo::physics::WorldPtr RTTGazeboEmbedded::getWorldPtr()
{
    return this->world;
}

void RTTGazeboEmbedded::addPlugin(const std::string& filename)
{
    gazebo::addPlugin(filename);
}
void RTTGazeboEmbedded::setWorldFilePath(const std::string& file_path)
{
    if(std::ifstream(file_path))
        world_path = file_path;
    else
        RTT::log(RTT::Error) << "File "<<file_path<<"does not exists."<<RTT::endlog();
}
bool RTTGazeboEmbedded::configureHook()
{
    RTT::log(RTT::Info) << "Creating world at "<< world_path << RTT::endlog();

    if(! gazebo::setupServer(argv))
    {
        RTT::log(RTT::Error) << "Could not setupServer " << RTT::endlog();
        return false;
    }

    world = gazebo::loadWorld(world_path);
    if(!world) return false;

    RTT::log(RTT::Info) << "Binding world events" << RTT::endlog();
    world_begin =  gazebo::event::Events::ConnectWorldUpdateBegin(std::bind(&RTTGazeboEmbedded::writeToSim,this));
    world_end = gazebo::event::Events::ConnectWorldUpdateEnd(std::bind(&RTTGazeboEmbedded::readSim,this));

    return true;
}


bool RTTGazeboEmbedded::startHook()
{
    if(!run_th.joinable())
        run_th = std::thread(std::bind(&RTTGazeboEmbedded::runWorldForever,this));
    else{
        unPause();
    }
    return true;
}
void RTTGazeboEmbedded::runWorldForever()
{
    std::cout <<"\x1B[32m[[--- Gazebo running ---]]\033[0m"<<std::endl;
    gazebo::runWorld(world, 0); // runs forever
    std::cout <<"\x1B[32m[[--- Gazebo exiting runWorld() ---]]\033[0m"<<std::endl;
}
void RTTGazeboEmbedded::updateHook()
{
    if(use_rtt_sync)
    {
        gazebo::event::Events::pause.Signal(false);
        go_sem.signal();
    }
    return;
}
void RTTGazeboEmbedded::pause()
{
    std::cout <<"\x1B[32m[[--- Pausing Simulation ---]]\033[0m"<<std::endl;
    gazebo::event::Events::pause.Signal(true);
}
void RTTGazeboEmbedded::unPause()
{
    std::cout <<"\x1B[32m[[--- Unpausing Simulation ---]]\033[0m"<<std::endl;
    gazebo::event::Events::pause.Signal(false);
}

void RTTGazeboEmbedded::stopHook()
{
    pause();
}

void RTTGazeboEmbedded::checkClientConnections()
{
    if(getPeerList().size() && 
        getPeerList().size() != client_map.size())
    {
        for(auto p : getPeerList())
        {
            if(client_map.find(p) == client_map.end())
            {
                if(getPeer(p)->provides("gazebo") && 
                    getPeer(p)->provides("gazebo")->hasOperation("read") &&
                    getPeer(p)->provides("gazebo")->hasOperation("write")
                )
                {
                    log(Info) << "Adding new client "<<p<<endlog();
                    client_map[p] = ClientConnection(getPeer(p)->provides("gazebo")->getOperation("read"),
                                                    getPeer(p)->provides("gazebo")->getOperation("write"));
                }
            }
        }
    }
    
    auto it = std::begin(client_map);
    while(it != std::end(client_map))
    {
        if(!it->second.read_callback.ready() || 
            !it->second.write_callback.ready())
        {
            log(Warning) << "Removing broken connection with client "<<it->first<<endlog();
            it = client_map.erase(it);
        }else
            ++it;
    }
    return;
}

void RTTGazeboEmbedded::writeToSim()
{
    checkClientConnections();
    
    for(auto c : client_map)
        c.second.write_handle = c.second.write_callback.send();
    
    for(auto c : client_map)
        c.second.write_handle.collect();
    
    port_sync.write(true);
}

void RTTGazeboEmbedded::readSim()
{
    for(auto c : client_map)
        c.second.read_handle = c.second.read_callback.send();
    
    for(auto c : client_map)
        c.second.read_handle.collect();

    if(use_rtt_sync)
        go_sem.wait();
}
void RTTGazeboEmbedded::cleanupHook()
{
    this->unPause();
    std::cout <<"\x1B[32m[[--- Stoping Simulation ---]]\033[0m"<<std::endl;
    gazebo::event::Events::stop.Signal();
    gazebo::runWorld(this->world, 1);
    this->run_th.join();
    std::cout <<"\x1B[32m[[--- Gazebo Shutdown... ---]]\033[0m"<<std::endl;
    //NOTE: This crashes as gazebo is running is a thread
    gazebo::shutdown();

    std::cout <<"\x1B[32m[[--- Exiting Gazebo ---]]\033[0m"<<std::endl;
}

bool RTTGazeboEmbedded::readyROSService(std_srvs::EmptyRequest& req,std_srvs::EmptyResponse& res)
{
    return true;
}

ORO_CREATE_COMPONENT(RTTGazeboEmbedded)
