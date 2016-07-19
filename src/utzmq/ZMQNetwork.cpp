/*
 * Ubitrack - Library for Ubiquitous Tracking
 * Copyright 2006, Technische Universitaet Muenchen, and individual
 * contributors as indicated by the @authors tag. See the
 * copyright.txt in the distribution for a full listing of individual
 * contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */

#include <boost/archive/binary_iarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

#include "ZMQNetwork.h"

#include <sstream>
#include <iostream>
#include <istream>


#include <utUtil/OS.h>
#include <utDataflow/ComponentFactory.h>
#include <utMeasurement/Measurement.h>
#include <utVision/Image.h>

#include <boost/array.hpp>

#include <log4cpp/Category.hh>


namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQNetwork" ) );

// static zmq context as singleton
boost::shared_ptr<zmq::context_t> NetworkModule::m_context;
boost::atomic<int> NetworkModule::m_context_users(0);

NetworkModule::NetworkModule( const NetworkModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph > pConfig, FactoryHelper* pFactory )
    : Module< NetworkModuleKey, NetworkComponentKey, NetworkModule, NetworkComponentBase >( moduleKey, pFactory )
    , m_bindTo(false)
    , m_fixTimestamp(true)
    , m_verbose(true)
    , m_has_pushsink(false)
    , m_has_pushsource(false)
    , m_msgwait_timeout(100) // microseconds -- or are they milliseconds ?
{
    if ( pConfig->m_DataflowAttributes.hasAttribute( "bindTo" ) )
    {
        m_bindTo = pConfig->m_DataflowAttributes.getAttributeString( "bindTo" ) == "true";
    }
    if ( pConfig->m_DataflowAttributes.hasAttribute( "fixTimestamp" ) )
    {
        m_fixTimestamp = pConfig->m_DataflowAttributes.getAttributeString( "fixTimestamp" ) == "true";
    }
    if ( pConfig->m_DataflowAttributes.hasAttribute( "verbose" ) )
    {
        m_verbose = pConfig->m_DataflowAttributes.getAttributeString( "verbose" ) == "true";
    }


    stopModule();
}


NetworkModule::~NetworkModule()
{
	stopModule();
}

void NetworkModule::startModule()
{
	if ( !m_running )
	{

        LOG4CPP_DEBUG( logger, "Check ZMQNetwork module configuration: " << m_moduleKey.get() );


        ComponentList allComponents( getAllComponents() );

        m_has_pushsink = false;
        m_has_pushsource = false;

        for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
            NetworkComponentBase::ComponentType t = (*it)->getComponentType();
            switch(t) {
            case NetworkComponentBase::PUSH_SINK:
                if (m_has_pushsource) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork has Sink and Source on one socket.");
                }
                m_has_pushsink = true;
                break;
            case NetworkComponentBase::PUSH_SOURCE:
                if (m_has_pushsink) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork has Sink and Source on one socket.");
                }
                m_has_pushsource = true;
                break;
            default:
                break;
            }
        }

        if (!m_has_pushsink && !m_has_pushsource) {
            UBITRACK_THROW("Configuration Error: ZMQNetwork has no Sinks or Sources.");
        }

        m_running = true;

        LOG4CPP_INFO( logger, "Starting ZMQNetwork module: " << m_moduleKey.get() );

        int socket_type = ZMQ_SUB;
        if (m_has_pushsink) {
            socket_type = ZMQ_PUB;
        }
		if (m_context_users.fetch_add(1, boost::memory_order_relaxed) == 0) {
			boost::atomic_thread_fence(boost::memory_order_acquire);
			LOG4CPP_INFO( logger, "ZMQ Context create" );
			m_context.reset( new zmq::context_t(ZMQNETWORK_IOTHREADS));
		}
        m_socket = boost::shared_ptr< zmq::socket_t >( new zmq::socket_t(*m_context, socket_type) );


        try {
            if (m_bindTo) {
                m_socket->bind(m_moduleKey.get().c_str());
            } else {
                m_socket->connect(m_moduleKey.get().c_str());
            }
            // only for ZMQ_SUB sockets
            if (m_has_pushsource) {
                m_socket->setsockopt(ZMQ_SUBSCRIBE, "", 0);
            }
        }
        catch (zmq::error_t &e) {
            std::ostringstream log;
            log << "Error initializing ZMQNetwork: " << std::endl;
            log << "address: "  << m_moduleKey.get() << std::endl;
            log << e.what() << std::endl;
            LOG4CPP_ERROR( logger, log.str() );
            UBITRACK_THROW("Error Initializing ZMQNetwork");
        }

        for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
            (*it)->setupComponent(m_socket);
        }

        if (m_has_pushsource) {
            // network thread runs until module is stopped
            LOG4CPP_DEBUG( logger, "Starting network receiver thread" );
            m_NetworkThread = boost::shared_ptr< boost::thread >( new boost::thread( boost::bind( &NetworkModule::startReceiver, this ) ) );
        }

        LOG4CPP_DEBUG( logger, "ZMQ Network module started" );
	}
}

void NetworkModule::stopModule()
{

	if ( m_running )
	{

		m_running = false;
        LOG4CPP_NOTICE( logger, "Stopping ZMQ Network Module" );

        if (m_has_pushsource) {
            // wait for thread
            m_NetworkThread->join();
            m_NetworkThread.reset();
        }

        ComponentList allComponents( getAllComponents() );

        for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
            (*it)->teardownComponent();
        }
        m_socket.reset();
		if (m_context_users.fetch_sub(1, boost::memory_order_release) == 1) {
			boost::atomic_thread_fence(boost::memory_order_acquire);
			LOG4CPP_INFO( logger, "ZMQ Context close" );
			// not all versions of zmq.hpp have a close method, but they should close when the instance is deleted...
			//m_context->close();
			m_context.reset();
		}
	}
    LOG4CPP_DEBUG( logger, "ZMQ Network Stopped" );
}

void NetworkModule::receiverThread() {
    zmq::pollitem_t pollitems[1];
    pollitems[0].socket = *m_socket;
    pollitems[0].events = ZMQ_POLLIN;

    // main loop
    while (m_running) {
        // wait for messages
        try {
            zmq::poll (pollitems, 1, m_msgwait_timeout);
        } catch (zmq::error_t &e) {
            std::ostringstream log;
            log << "Error polling zmq socket: " << std::endl;
            log << "address: "  << m_moduleKey.get() << std::endl;
            log << e.what() << std::endl;
            LOG4CPP_ERROR( logger, log.str() );

            Ubitrack::Util::sleep(100);

            continue;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {

            // zmq_poll has returned
            Measurement::Timestamp ts = Measurement::now();

            zmq::message_t message;
            int flags = 0; // ZMQ_NOBLOCK
            bool rc;

            if((rc = m_socket->recv(&message, flags)) == true) {
                if (m_verbose) {
                    LOG4CPP_DEBUG( logger, "Received " << message.size() << " bytes" );
                }
                try
                {
                   
                    					
					LOG4CPP_TRACE(logger, "data size: " << message.size());

					typedef boost::iostreams::basic_array_source<char> Device;
					boost::iostreams::stream_buffer<Device> buffer((char*)message.data(), message.size());
					boost::archive::binary_iarchive ar_message(buffer);


                    // parse packet
                    std::string name;					
                    ar_message >> name;
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Message for component " << name );
                    }

                    NetworkComponentKey key( name );

                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );
                        comp->parse( ar_message, ts );
                    }
                    else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQSink is sending with id=\"" << name << "\", found no corresponding ZMQSource pattern with same id."  );
                    }
                }
                catch ( const std::exception& e )
                {
                    LOG4CPP_ERROR( logger, "Caught exception " << e.what() );
                }
            } else {
                LOG4CPP_ERROR( logger, "Error receiving zmq message" );
            }
            pollitems[0].revents = 0;
        }
    }
}

boost::shared_ptr< NetworkComponentBase > NetworkModule::createComponent( const std::string& type, const std::string& name,
    boost::shared_ptr< Graph::UTQLSubgraph > config, const NetworkModule::ComponentKey& key, NetworkModule* pModule )
{

	if ( type == "ZMQSourceEvent" ) // @todo should be button to be consistent or rename buttopn after all
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Button >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceSkalar" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Distance >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceDistance" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Distance >( name, config, key, pModule ) );

	else if ( type == "ZMQSourcePosition2D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Position2D >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Position >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Pose >( name, config, key, pModule ) );

    else if ( type == "ZMQSourceErrorPosition2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPosition2 >( name, config, key, pModule ) );
    else if ( type == "ZMQSourceErrorPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPosition >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceErrorPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPose >( name, config, key, pModule ) );

	else if ( type == "ZMQSourceRotation" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Rotation >( name, config, key, pModule ) );

	else if ( type == "ZMQSourceMatrix3x3" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix3x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix4x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

    else if ( type == "ZMQSourceVector4D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Vector4D >( name, config, key, pModule ) );
    else if ( type == "ZMQSourceVector8D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Vector8D >( name, config, key, pModule ) );

    else if ( type == "ZMQSourceEventList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ButtonList >( name, config, key, pModule ) );
    else if ( type == "ZMQSourceDistanceList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::DistanceList >( name, config, key, pModule ) );

	else if ( type == "ZMQSourcePositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::PositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::PoseList >( name, config, key, pModule ) );


    else if ( type == "ZMQSourceErrorPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQSourceErrorPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQSourceErrorPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPoseList >( name, config, key, pModule ) );


	else if ( type == "ZMQSourceCameraIntrinsics" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::CameraIntrinsics >( name, config, key, pModule ) );

    else if ( type == "ZMQSourceRotationVelocity" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::RotationVelocity >( name, config, key, pModule ) );

    else if (type == "ZMQSourceImage")
		return boost::shared_ptr< NetworkComponentBase >(new PushSourceComponent< Measurement::ImageMeasurement >(name, config, key, pModule));


    // sinks
    else if ( type == "ZMQSinkEvent" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Button >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkDistance" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Distance >( name, config, key, pModule ) );


    else if ( type == "ZMQSinkPosition2D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Position2D >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Position >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Pose >( name, config, key, pModule ) );


    else if ( type == "ZMQSinkErrorPosition2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPosition2 >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkErrorPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPosition >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkErrorPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPose >( name, config, key, pModule ) );


    else if ( type == "ZMQSinkRotation" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Rotation >( name, config, key, pModule ) );

    else if ( type == "ZMQSinkMatrix3x3" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkMatrix3x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkMatrix4x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

    else if ( type == "ZMQSinkVector4D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Vector4D >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkVector8D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Vector8D >( name, config, key, pModule ) );


    else if ( type == "ZMQSinkEventList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ButtonList >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkDistanceList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::DistanceList >( name, config, key, pModule ) );

    else if ( type == "ZMQSinkPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::PositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::PoseList >( name, config, key, pModule ) );


    else if ( type == "ZMQSinkErrorPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkErrorPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQSinkErrorPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPoseList >( name, config, key, pModule ) );

    else if ( type == "ZMQSinkCameraIntrinsics" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::CameraIntrinsics >( name, config, key, pModule ) );

    else if ( type == "ZMQSinkRotationVelocity" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::RotationVelocity >( name, config, key, pModule ) );

    else if (type == "ZMQSinkImage")
		return boost::shared_ptr< NetworkComponentBase >(new PushSinkComponent< Measurement::ImageMeasurement >(name, config, key, pModule));

    UBITRACK_THROW( "Class " + type + " not supported by ZMQNetwork module." );
}


// register module at factory
UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {

	// create list of supported types
    std::vector< std::string > NetworkComponents;

    NetworkComponents.push_back( "ZMQSourcePose" );
    NetworkComponents.push_back( "ZMQSourceErrorPose" );
    NetworkComponents.push_back( "ZMQSourceRotation" );
    NetworkComponents.push_back( "ZMQSourcePosition" );
    NetworkComponents.push_back( "ZMQSourcePosition2D" );
    NetworkComponents.push_back( "ZMQSourcePoseList" );
    NetworkComponents.push_back( "ZMQSourcePositionList" );
    NetworkComponents.push_back( "ZMQSourcePositionList2" );
    NetworkComponents.push_back( "ZMQSourceEvent" );
    NetworkComponents.push_back( "ZMQSourceMatrix3x3" );
    NetworkComponents.push_back( "ZMQSourceMatrix3x4" );
    NetworkComponents.push_back( "ZMQSourceMatrix4x4" );
    NetworkComponents.push_back( "ZMQSourceDistance" );

    NetworkComponents.push_back( "ZMQSourceVector4D" );
    NetworkComponents.push_back( "ZMQSourceVector8D" );
    NetworkComponents.push_back( "ZMQSourceRotationVelocity" );
    NetworkComponents.push_back( "ZMQSourceErrorPosition" );
    NetworkComponents.push_back( "ZMQSourceDistanceList" );
    NetworkComponents.push_back( "ZMQSourceErrorPositionList2" );
    NetworkComponents.push_back( "ZMQSourceErrorPositionList" );
    NetworkComponents.push_back( "ZMQSourceCameraIntrinsics" );
	NetworkComponents.push_back("ZMQSourceImage");

    NetworkComponents.push_back( "ZMQSinkPose" );
    NetworkComponents.push_back( "ZMQSinkErrorPose" );
    NetworkComponents.push_back( "ZMQSinkPosition" );
    NetworkComponents.push_back( "ZMQSinkPosition2D" );
    NetworkComponents.push_back( "ZMQSinkRotation" );
    NetworkComponents.push_back( "ZMQSinkPoseList" );
    NetworkComponents.push_back( "ZMQSinkPositionList" );
    NetworkComponents.push_back( "ZMQSinkPositionList2" );
    NetworkComponents.push_back( "ZMQSinkEvent" );
    NetworkComponents.push_back( "ZMQSinkMatrix3x3" );
    NetworkComponents.push_back( "ZMQSinkMatrix3x4" );
    NetworkComponents.push_back( "ZMQSinkMatrix4x4" );
    NetworkComponents.push_back( "ZMQSinkDistance" );

    NetworkComponents.push_back( "ZMQSinkVector4D" );
    NetworkComponents.push_back( "ZMQSinkVector8D" );
    NetworkComponents.push_back( "ZMQSinkRotationVelocity" );
    NetworkComponents.push_back( "ZMQSinkErrorPosition" );
    NetworkComponents.push_back( "ZMQSinkDistanceList" );
    NetworkComponents.push_back( "ZMQSinkErrorPositionList2" );
    NetworkComponents.push_back( "ZMQSinkErrorPositionList" );
    NetworkComponents.push_back( "ZMQSinkCameraIntrinsics" );
	NetworkComponents.push_back("ZMQSinkImage");

    cf->registerModule< NetworkModule >( NetworkComponents );
}

} } // namespace Ubitrack::Drivers
