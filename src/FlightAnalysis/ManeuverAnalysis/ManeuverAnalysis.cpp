/*
 * ManeuverAnalysis.cpp
 *
 *  Created on: Aug 6, 2018
 *      Author: simonyu
 */

#include <string>

#include "uavAP/FlightAnalysis/ManeuverAnalysis/ManeuverAnalysis.h"

ManeuverAnalysis::ManeuverAnalysis() :
		analysis_(false), collectInit_(false), counter_(0), maneuver_(Maneuvers::GEOFENCING)
{
}


bool
ManeuverAnalysis::configure(const Configuration& config)
{
	PropertyMapper<Configuration> pm(config);
	std::string maneuver;

	pm.add("log_path", logPath_, true);

	if (pm.add("maneuver", maneuver, false))
	{
		maneuver_ = EnumMap<Maneuvers>::convert(maneuver);
	}

	collectInit_ = false;
	counter_ = 0;

	return pm.map();
}

bool
ManeuverAnalysis::run(RunStage stage)
{
	switch (stage)
	{
	case RunStage::INIT:
	{
		if (!checkIsSet<IPC, IScheduler, DataPresentation>())
		{
			CPSLOG_ERROR << "ManeuverAnalysis: Missing dependencies.";
			return true;
		}

		break;
	}
	case RunStage::NORMAL:
	{
		auto ipc = get<IPC>();

		sensorDataSubscription_ = ipc->subscribe<SensorData>("sensor_data",
				std::bind(&ManeuverAnalysis::onSensorData, this, std::placeholders::_1));

		if (!sensorDataSubscription_.connected())
		{
			APLOG_ERROR << "ManeuverAnalysis: Sensor Data Subscription Missing.";
			return true;
		}

		controllerOutputSubscription_ = ipc->subscribe<ControllerOutput>("controller_output",
				std::bind(&ManeuverAnalysis::onControllerOutput, this, std::placeholders::_1));

		if (!controllerOutputSubscription_.connected())
		{
			APLOG_ERROR << "ManeuverAnalysis: Controller Output Subscription Missing.";
			return true;
		}

		maneuverAnalysisSubscription_ = ipc->subscribe<bool>("maneuver_analysis",
				std::bind(&ManeuverAnalysis::onManeuverAnalysis, this, std::placeholders::_1));

		if (!maneuverAnalysisSubscription_.connected())
		{
			APLOG_ERROR << "ManeuverAnalysis: Maneuver Analysis Subscription Missing.";
			return true;
		}

		maneuverAnalysisStatusSubscription_ = ipc->subscribeOnPackets("maneuver_analysis_status",
				std::bind(&ManeuverAnalysis::onManeuverAnalysisStatus, this,
						std::placeholders::_1));

		if (!maneuverAnalysisStatusSubscription_.connected())
		{
			APLOG_ERROR << "ManeuverAnalysis: Maneuver Analysis Status Subscription Missing.";
			return true;
		}

		break;
	}
	default:
	{
		break;
	}
	}

	return false;
}

void
ManeuverAnalysis::onSensorData(const SensorData& data)
{
	Lock maneuverAnalysisLock(maneuverAnalysisMutex_);
	bool analysis = analysis_;
	maneuverAnalysisLock.unlock();

	Lock maneuverAnalysisStatusLock(maneuverAnalysisStatusMutex_);
	ManeuverAnalysisStatus analysisStatus = analysisStatus_;
	maneuverAnalysisStatusLock.unlock();

	if (analysis && !collectInit_)
	{
		collectStateInit(data, analysisStatus.maneuver, analysisStatus.interrupted);
	}
	else if (analysis && collectInit_)
	{
		collectStateNormal(data, analysisStatus.analysis);
	}
	else if (!analysis && collectInit_)
	{
		collectStateFinal(sensorData_);
	}
}

void
ManeuverAnalysis::onManeuverAnalysis(const bool& analysis)
{
	Lock lock(maneuverAnalysisMutex_);
	analysis_ = analysis;
	lock.unlock();
}

void
ManeuverAnalysis::onManeuverAnalysisStatus(const Packet& status)
{
	if (auto dp = get<DataPresentation>())
	{
		Lock lock(maneuverAnalysisStatusMutex_);
		analysisStatus_ = dp->deserialize<ManeuverAnalysisStatus>(status);
		lock.unlock();
	}
}

void
ManeuverAnalysis::onControllerOutput(const ControllerOutput& output)
{
	Lock lock(controllerOutputMutex_);
	controllerOutput_ = output;
	lock.unlock();
}

void
ManeuverAnalysis::collectStateInit(const SensorData& data, const std::string& maneuver,
		const bool& interrupted)
{
	APLOG_DEBUG << "ManeuverAnalysis::collectStateInit.";

	if (!interrupted)
	{
		counter_++;
	}

	std::string maneuverName = std::to_string(counter_) + "_" + maneuver;
	std::string logFileName = logPath_ + maneuverName + ".log";
	auto t = std::chrono::system_clock::to_time_t(data.timestamp);
	std::string time = std::ctime(&t);

	logFile_.open(logFileName);
	logFile_.precision(15);

	if (!logFile_.is_open())
	{
		APLOG_ERROR << "ManeuverAnalysis: Failed to Open Log File. Cannot Log.";
		return;
	}

	logFile_ << maneuverName << std::endl;
	logFile_ << time << std::endl;
	logFile_ << "collectStateInit:" << std::endl;

	switch (maneuver_)
	{
	case Maneuvers::GEOFENCING:
	{
		collectGeofencing(data, CollectStates::INIT);
		break;
	}
	case Maneuvers::ADVANCED_CONTROL:
	{
		collectAdvancedControl(data, CollectStates::INIT);
		break;
	}
	case Maneuvers::FLIGHT_TESTING:
	{
		collectFlightTesting(data, CollectStates::INIT);
		break;
	}
	default:
	{
		break;
	}
	}

	logFile_ << "collectStateNormal:" << std::endl;

	collectInit_ = true;
}

void
ManeuverAnalysis::collectStateNormal(const SensorData& data, const bool& analysis)
{
	if (!analysis)
	{
		return;
	}

	APLOG_DEBUG << "ManeuverAnalysis::collectStateNormal.";

	if (!logFile_.is_open())
	{
		APLOG_ERROR << "ManeuverAnalysis: Failed to Open Log File. Cannot Log.";
		return;
	}

	switch (maneuver_)
	{
	case Maneuvers::GEOFENCING:
	{
		collectGeofencing(data, CollectStates::NORMAL);
		break;
	}
	case Maneuvers::ADVANCED_CONTROL:
	{
		collectAdvancedControl(data, CollectStates::NORMAL);
		break;
	}
	case Maneuvers::FLIGHT_TESTING:
	{
		collectFlightTesting(data, CollectStates::NORMAL);
		break;
	}
	default:
	{
		break;
	}
	}
}

void
ManeuverAnalysis::collectStateFinal(const SensorData& data)
{
	APLOG_DEBUG << "ManeuverAnalysis::collectStateFinal.";

	if (!logFile_.is_open())
	{
		APLOG_ERROR << "ManeuverAnalysis: Failed to Open Log File. Cannot Log.";
		return;
	}

	logFile_ << "collectStateFinal:" << std::endl;

	switch (maneuver_)
	{
	case Maneuvers::GEOFENCING:
	{
		collectGeofencing(data, CollectStates::FINAL);
		break;
	}
	case Maneuvers::ADVANCED_CONTROL:
	{
		collectAdvancedControl(data, CollectStates::FINAL);
		break;
	}
	case Maneuvers::FLIGHT_TESTING:
	{
		collectFlightTesting(data, CollectStates::FINAL);
		break;
	}
	default:
	{
		break;
	}
	}

	logFile_.close();

	collectInit_ = false;
}

void
ManeuverAnalysis::collectGeofencing(const SensorData& data, const CollectStates& states)
{
	switch (states)
	{
	case CollectStates::INIT:
	{
		logFile_ << "Position E (m)" << "	" << "Position N (m)" << "	" << "Roll Angle (Rad)" << "	"
				<< "Yaw Angle (Rad)" << "	" << "Ground Speed (m/s)" << "	" << "Roll Rate (Rad/s)"
				<< std::endl;

		logFile_ << data.position.x() << "	" << data.position.y() << "	" << data.attitude.x() << "	"
				<< data.attitude.z() << "	" << data.groundSpeed << "	" << data.angularRate.x()
				<< std::endl;

		break;
	}
	case CollectStates::NORMAL:
	{
		logFile_ << data.position.x() << "	" << data.position.y() << "	" << data.attitude.x() << "	"
				<< data.attitude.z() << "	" << data.groundSpeed << "	" << data.angularRate.x()
				<< std::endl;

		break;
	}
	case CollectStates::FINAL:
	{
		break;
	}
	default:
	{
		break;
	}
	}
}

void
ManeuverAnalysis::collectAdvancedControl(const SensorData& data, const CollectStates& states)
{
	switch (states)
	{
	case CollectStates::INIT:
	{
		logFile_ << "Velocity E (m/s)" << "	" << "Velocity N (m/s)" << "	" << "Velocity U (m/s)"
				<< "	" << "Air Speed (m/s)" << "	" << "Roll Angle (Rad)" << "	"
				<< "Pitch Angle (Rad)" << "	" << "Yaw Angle (Rad)" << "	" << "Battery Voltage (V)"
				<< "	" << "Battery Current (A)" << "	" << "Throttle Level (%)" << "	"
				<< "Motor Speed (RPM)" << std::endl;

		logFile_ << data.velocity.x() << "	" << data.velocity.y() << "	" << data.velocity.z() << "	"
				<< data.airSpeed << "	" << data.attitude.x() << "	" << data.attitude.y() << "	"
				<< data.attitude.z() << "	" << data.batteryVoltage << "	" << data.batteryCurrent
				<< "	" << data.throttle << "	" << data.rpm << std::endl;

		break;
	}
	case CollectStates::NORMAL:
	{
		logFile_ << data.velocity.x() << "	" << data.velocity.y() << "	" << data.velocity.z() << "	"
				<< data.airSpeed << "	" << data.attitude.x() << "	" << data.attitude.y() << "	"
				<< data.attitude.z() << "	" << data.batteryVoltage << "	" << data.batteryCurrent
				<< "	" << data.throttle << "	" << data.rpm << std::endl;

		break;
	}
	case CollectStates::FINAL:
	{
		break;
	}
	default:
	{
		break;
	}
	}
}

ManeuverAnalysis::~ManeuverAnalysis()
{
	if (logFile_.is_open())
		logFile_.close();
}

void
ManeuverAnalysis::collectFlightTesting(const SensorData& data, const CollectStates& states)
{
	Lock lock(controllerOutputMutex_);
	ControllerOutput controllerOutput = controllerOutput_;
	lock.unlock();

	switch (states)
	{
	case CollectStates::INIT:
	{
		logFile_ << "Position x (m)" << "	" << "Position y (m)" << "	" << "Position z (m)" << "	"
				<< "Velocity x (m/s)" << "	" << "Velocity y (m/s)" << "	" << "Velocity z (m/s)"
				<< "	" << "Acceleration x (m/s)" << "	" << "Acceleration y (m/s)" << "	"
				<< "Acceleration z (m/s)" << "	" << "Euler Angles phi (Rad)" << "	"
				<< "Euler Angles theta (Rad)" << "	" << "Euler Angles psi (Rad)" << "	"
				<< "Rotation Rate x (Rad/s)" << "	" << "Rotation Rate y (Rad/s)" << "	"
				<< "Rotation Rate z (Rad/s)" << "	" << "Timestamp (UTC)" << "	" << "Air Speed (m/s)"
				<< "	" << "Ground Speed (m/s)" << "	" << "Battery Voltage (V)" << "	"
				<< "Battery Current (A)" << "	" << "Aileron Level (Deg)" << "	"
				<< "Elevator Level (Deg)" << "	" << "Rudder Level (Deg)" << "	" << "Throttle Level (%)"
				<< "	" << "Motor Speed (RPM)" << "	" << "Roll Output (%)" << "	"
				<< "Pitch Output (%)" << "	" << "Yaw Output (%)" << "	" << "Throttle Output (%)"
				<< std::endl;

		logFile_ << data.position.x() << "	" << data.position.y() << "	" << data.position.z() << "	"
				<< data.velocity.x() << "	" << data.velocity.y() << "	" << data.velocity.z() << "	"
				<< data.acceleration.x() << "	" << data.acceleration.y() << "	"
				<< data.acceleration.z() << "	" << data.attitude.x() << "	" << data.attitude.y()
				<< "	" << data.attitude.z() << "	" << data.angularRate.x() << "	"
				<< data.angularRate.y() << "	" << data.angularRate.z() << "	"
				<< data.timestamp.time_since_epoch().count() << "	" << data.airSpeed << "	"
				<< data.groundSpeed << "	" << data.batteryVoltage << "	" << data.batteryCurrent
				<< "	" << data.aileron << "	" << data.elevator << "	" << data.rudder << "	"
				<< data.throttle << "	" << data.rpm << "	" << controllerOutput.rollOutput << "	"
				<< controllerOutput.pitchOutput << "	" << controllerOutput.yawOutput << "	"
				<< controllerOutput.throttleOutput << std::endl;

		logFile_ << "collectStateNormal:" << std::endl;

		break;
	}
	case CollectStates::NORMAL:
	{
		logFile_ << data.position.x() << "	" << data.position.y() << "	" << data.position.z() << "	"
				<< data.velocity.x() << "	" << data.velocity.y() << "	" << data.velocity.z() << "	"
				<< data.acceleration.x() << "	" << data.acceleration.y() << "	"
				<< data.acceleration.z() << "	" << data.attitude.x() << "	" << data.attitude.y()
				<< "	" << data.attitude.z() << "	" << data.angularRate.x() << "	"
				<< data.angularRate.y() << "	" << data.angularRate.z() << "	"
				<< data.timestamp.time_since_epoch().count() << "	" << data.airSpeed << "	"
				<< data.groundSpeed << "	" << data.batteryVoltage << "	" << data.batteryCurrent
				<< "	" << data.aileron << "	" << data.elevator << "	" << data.rudder << "	"
				<< data.throttle << "	" << data.rpm << "	" << controllerOutput.rollOutput << "	"
				<< controllerOutput.pitchOutput << "	" << controllerOutput.yawOutput << "	"
				<< controllerOutput.throttleOutput << std::endl;

		break;
	}
	case CollectStates::FINAL:
	{
		logFile_ << data.position.x() << "	" << data.position.y() << "	" << data.position.z() << "	"
				<< data.velocity.x() << "	" << data.velocity.y() << "	" << data.velocity.z() << "	"
				<< data.acceleration.x() << "	" << data.acceleration.y() << "	"
				<< data.acceleration.z() << "	" << data.attitude.x() << "	" << data.attitude.y()
				<< "	" << data.attitude.z() << "	" << data.angularRate.x() << "	"
				<< data.angularRate.y() << "	" << data.angularRate.z() << "	"
				<< data.timestamp.time_since_epoch().count() << "	" << data.airSpeed << "	"
				<< data.groundSpeed << "	" << data.batteryVoltage << "	" << data.batteryCurrent
				<< "	" << data.aileron << "	" << data.elevator << "	" << data.rudder << "	"
				<< data.throttle << "	" << data.rpm << "	" << controllerOutput.rollOutput << "	"
				<< controllerOutput.pitchOutput << "	" << controllerOutput.yawOutput << "	"
				<< controllerOutput.throttleOutput << std::endl;

		break;
	}
	default:
	{
		break;
	}
	}
}
