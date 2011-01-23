// kate: indent-mode cstyle; indent-width 4; tab-width 4; space-indent false;
// vim:ai ts=4 et
#include "WorldModel.hpp"

#include <iostream>
#include <QObject>
#include <QString>
#include <vector>
#include <boost/foreach.hpp>

#include <Configuration.hpp>
#include <Constants.hpp>
#include <Utils.hpp>

// different options for ball model here
#include "RBPFBallModel.hpp"
//#include "EKFBallModel.hpp" // currently broken

using namespace std;
using namespace Modeling;
using namespace Utils;
using namespace google::protobuf;

WorldModel::WorldModel(SystemState *state, Configuration *config) :
	_robotConfig(config),
	_state(state),
	_selfPlayers(Robots_Per_Team),
	_oppPlayers(Robots_Per_Team)
{
	_ballModel = new RBPFBallModel(&_robotMap, config);
}

WorldModel::~WorldModel()
{
	delete _ballModel;
}

void WorldModel::run(bool blueTeam, const std::vector<const SSL_DetectionFrame *> &rawVision)
{
	// internal verbosity flag for debugging
	bool verbose = false;

	if (verbose) cout << "In WorldModel::run()" << endl;

	// Add vision packets
	uint64_t curTime = 0;
	if (verbose) cout << "Adding vision packets" << endl;
	BOOST_FOREACH(const SSL_DetectionFrame* vision, rawVision)
	{
		//FIXME - This time is not usable here.  It is in the vision computer's clock.
		uint64_t timestamp = vision->t_capture() * 1000000.0;
		curTime = max(curTime, timestamp);
		
		// determine team
		const RepeatedPtrField<SSL_DetectionRobot> *self, *opp;
		if (blueTeam)
		{
			self = &vision->robots_blue();
			opp = &vision->robots_yellow();
		} else {
			self = &vision->robots_yellow();
			opp = &vision->robots_blue();
		}

		// add ball observation
		BOOST_FOREACH(const SSL_DetectionBall &ball, vision->balls())
		{
			Geometry2d::Point pos(ball.x() / 1000.0f, ball.y() / 1000.0f);
			_ballModel->observation(timestamp, pos, BallModel::VISION);
		}

		// add robot observations
		BOOST_FOREACH(const SSL_DetectionRobot &robot, *self)
		{
			addRobotObseration(robot, timestamp, _selfPlayers);
		}
		BOOST_FOREACH(const SSL_DetectionRobot &robot, *opp)
		{
			addRobotObseration(robot, timestamp, _oppPlayers);
		}
	}

	// get robot data from return packets
	if (verbose) cout << "Adding robot rx data" << endl;
	BOOST_FOREACH(OurRobot *robot, _state->self) {
		addRobotRxData(robot);
	}

	// Robots perform updates
	if (verbose) cout << "updating players" << endl;
	updateRobots(_selfPlayers, curTime);
	updateRobots(_oppPlayers, curTime);

	// Ball sensor
	//FIXME - Detect broken ball sensors and spurious triggers (from spinning or collision)
	BOOST_FOREACH(OurRobot *robot, _state->self)
	{
		robot->hasBall = robot->radioRx.ball();
	}
	
	// Copy robot data out of models into state
	if (verbose) cout << "copying out data for robots" << endl;
	// by default, sets robots to not visible
	BOOST_FOREACH(Robot *robot, _state->self)
	{
		robot->visible = false;
	}
	BOOST_FOREACH(Robot *robot, _state->opp)
	{
		robot->visible = false;
	}
	
	BOOST_FOREACH(const RobotModel::shared& robot, _selfPlayers)
	{
		if (robot) {
			int i = robot->shell();
			_state->self[i]->visible = true;
			_state->self[i]->pos = robot->pos();
			_state->self[i]->vel = robot->vel();
			_state->self[i]->angle = robot->angle();
			_state->self[i]->angleVel = robot->angleVel();
		}
	}
	BOOST_FOREACH(const RobotModel::shared& robot, _oppPlayers)
	{
		if (robot) {
			int i = robot->shell();
			_state->opp[i]->visible = true;
			_state->opp[i]->pos = robot->pos();
			_state->opp[i]->vel = robot->vel();
			_state->opp[i]->angle = robot->angle();
			_state->opp[i]->angleVel = robot->angleVel();
		}
	}

	// Store the robot models for use by the ball model
	_robotMap.clear();
	BOOST_FOREACH(const RobotModel::shared& model, _selfPlayers)
	{
		if (model)
		{
			_robotMap[model->shell()] = model;
		}
	}
	
	BOOST_FOREACH(const RobotModel::shared& model, _oppPlayers)
	{
		if (model)
		{
			_robotMap[model->shell() + OppOffset] = model;
		}
	}

	// add observations to the ball based on ball sensors and filtered robot positions
	if (verbose) cout << "adding ball observations for ball sensors" << endl;
	BOOST_FOREACH(RobotModel::shared robot, _selfPlayers)
	{
		if (robot) {
			//if a robot has the ball, we need to make an observation
#if 0
			//FIXME - They're breaking.  Turn this back on later.
			if (robot->valid(curTime) && robot->hasBall())
			{
				Geometry2d::Point offset = Geometry2d::Point::
						direction(robot->angle() * DegreesToRadians) *	Robot::Radius;

				_ballModel->observation(_state->timestamp, robot->pos() + offset, BallModel::BALL_SENSOR);
			}
#endif
		}
	}

	if (verbose) cout << "Updating ball" << endl;
	bool ballValid = _ballModel->valid(curTime); // checks whether should use prediction or last frame
	_ballModel->run(curTime);

	if (ballValid) {
		_state->ball.pos = _ballModel->pos;
		_state->ball.vel = _ballModel->vel;
		_state->ball.accel = _ballModel->accel;
	} else {
		// for invalid ball, just use
		_state->ball.pos = _ballModel->observedPos;
		_state->ball.vel = Geometry2d::Point();
		_state->ball.accel = Geometry2d::Point();
	}
	_state->ball.valid = true; // FIXME: always assume we have a valid ball

	if (verbose) cout << "At end of WorldModel::run()" << endl;
}

void WorldModel::addRobotObseration(const SSL_DetectionRobot &obs, uint64_t timestamp, RobotVector& players)
{
	int obs_shell = obs.robot_id();

	// try to add to an existing model, and return if we update something
	BOOST_FOREACH(RobotModel::shared& model, players) {
		if (model && model->shell() == obs_shell) {
			Geometry2d::Point pos(obs.x() / 1000.0f, obs.y() / 1000.0f);
			model->observation(timestamp, pos, obs.orientation() * RadiansToDegrees);
			return;
		}
	}

	// find an open slot - assumed that invalid models will have been removed already
	BOOST_FOREACH(RobotModel::shared& model, players) {
		if (!model) {
			model = RobotModel::shared(new RobotModel(&_robotConfig, obs_shell));
			Geometry2d::Point pos(obs.x() / 1000.0f, obs.y() / 1000.0f);
			model->observation(timestamp, pos, obs.orientation() * RadiansToDegrees);
			return;
		}
	}
}

void WorldModel::updateRobots(vector<RobotModel::shared>& players, uint64_t cur_time) {
	BOOST_FOREACH(RobotModel::shared& model, players) {
		if (model && model->valid(cur_time)) {
			model->update(cur_time);
		} else {
			model = RobotModel::shared();
		}
	}
}

void WorldModel::addRobotRxData(OurRobot *robot) {
	int shell = robot->shell();
	BOOST_FOREACH(RobotModel::shared& model, _selfPlayers) {
		if (model && model->shell() == shell) {
			model->hasBall(robot->radioRx.ball());
			return;
		}
	}
}
