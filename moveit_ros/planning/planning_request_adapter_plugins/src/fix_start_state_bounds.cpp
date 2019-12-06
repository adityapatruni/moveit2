/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/planning_request_adapter/planning_request_adapter.h>
#include <boost/math/constants/constants.hpp>
#include <moveit/trajectory_processing/trajectory_tools.h>
#include <moveit/robot_state/conversions.h>
#include <class_loader/class_loader.hpp>
#include <rclcpp/rclcpp.hpp>

namespace default_planner_request_adapters
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("moveit_ros.fix_start_state_bounds");

class FixStartStateBounds : public planning_request_adapter::PlanningRequestAdapter
{
public:
  static const std::string BOUNDS_PARAM_NAME;
  static const std::string DT_PARAM_NAME;

  void initialize(const rclcpp::Node::SharedPtr& node) override
  {
    node_ = node;
    if (!node_->get_parameter(BOUNDS_PARAM_NAME, bounds_dist_))
    {
      bounds_dist_ = 0.05;
      RCLCPP_INFO(LOGGER, "Param '%s' was not set. Using default value: %f", BOUNDS_PARAM_NAME.c_str(), bounds_dist_);
    }
    else
    {
      RCLCPP_INFO(LOGGER, "Param '%s' was set to %f", BOUNDS_PARAM_NAME.c_str(), bounds_dist_);
    }

    if (!node_->get_parameter(DT_PARAM_NAME, max_dt_offset_))
    {
      max_dt_offset_ = 0.5;
      RCLCPP_INFO(LOGGER, "Param '%s' was not set. Using default value: %f", DT_PARAM_NAME.c_str(), max_dt_offset_);
    }
    else
    {
      RCLCPP_INFO(LOGGER, "Param '%s' was set to %f", DT_PARAM_NAME.c_str(), max_dt_offset_);
    }
  }

  std::string getDescription() const override
  {
    return "Fix Start State Bounds";
  }

  bool adaptAndPlan(const PlannerFn& planner, const planning_scene::PlanningSceneConstPtr& planning_scene,
                    const planning_interface::MotionPlanRequest& req, planning_interface::MotionPlanResponse& res,
                    std::vector<std::size_t>& added_path_index) const override
  {
    RCLCPP_DEBUG(LOGGER, "Running '%s'", getDescription().c_str());

    // get the specified start state
    robot_state::RobotState start_state = planning_scene->getCurrentState();
    robot_state::robotStateMsgToRobotState(planning_scene->getTransforms(), req.start_state, start_state);

    const std::vector<const robot_model::JointModel*>& jmodels =
        planning_scene->getRobotModel()->hasJointModelGroup(req.group_name) ?
            planning_scene->getRobotModel()->getJointModelGroup(req.group_name)->getJointModels() :
            planning_scene->getRobotModel()->getJointModels();

    bool change_req = false;
    for (const moveit::core::JointModel* jm : jmodels)
    {
      // Check if we have a revolute, continuous joint. If we do, then we only need to make sure
      // it is within de model's declared bounds (usually -Pi, Pi), since the values wrap around.
      // It is possible that the encoder maintains values outside the range [-Pi, Pi], to inform
      // how many times the joint was wrapped. Because of this, we remember the offsets for continuous
      // joints, and we un-do them when the plan comes from the planner

      if (jm->getType() == robot_model::JointModel::REVOLUTE)
      {
        if (static_cast<const robot_model::RevoluteJointModel*>(jm)->isContinuous())
        {
          double initial = start_state.getJointPositions(jm)[0];
          start_state.enforceBounds(jm);
          double after = start_state.getJointPositions(jm)[0];
          if (fabs(initial - after) > std::numeric_limits<double>::epsilon())
            change_req = true;
        }
      }
      else
          // Normalize yaw; no offset needs to be remembered
          if (jm->getType() == robot_model::JointModel::PLANAR)
      {
        const double* p = start_state.getJointPositions(jm);
        double copy[3] = { p[0], p[1], p[2] };
        if (static_cast<const robot_model::PlanarJointModel*>(jm)->normalizeRotation(copy))
        {
          start_state.setJointPositions(jm, copy);
          change_req = true;
        }
      }
      else
          // Normalize quaternions
          if (jm->getType() == robot_model::JointModel::FLOATING)
      {
        const double* p = start_state.getJointPositions(jm);
        double copy[7] = { p[0], p[1], p[2], p[3], p[4], p[5], p[6] };
        if (static_cast<const robot_model::FloatingJointModel*>(jm)->normalizeRotation(copy))
        {
          start_state.setJointPositions(jm, copy);
          change_req = true;
        }
      }
    }

    // pointer to a prefix state we could possibly add, if we detect we have to make changes
    robot_state::RobotStatePtr prefix_state;
    for (const moveit::core::JointModel* jmodel : jmodels)
    {
      if (!start_state.satisfiesBounds(jmodel))
      {
        if (start_state.satisfiesBounds(jmodel, bounds_dist_))
        {
          if (!prefix_state)
            prefix_state.reset(new robot_state::RobotState(start_state));
          start_state.enforceBounds(jmodel);
          change_req = true;
          RCLCPP_INFO(LOGGER, "Starting state is just outside bounds (joint '%s'). Assuming within bounds.",
                      jmodel->getName().c_str());
        }
        else
        {
          std::stringstream joint_values;
          std::stringstream joint_bounds_low;
          std::stringstream joint_bounds_hi;
          const double* p = start_state.getJointPositions(jmodel);
          for (std::size_t k = 0; k < jmodel->getVariableCount(); ++k)
            joint_values << p[k] << " ";
          const robot_model::JointModel::Bounds& b = jmodel->getVariableBounds();
          for (const moveit::core::VariableBounds& variable_bounds : b)
          {
            joint_bounds_low << variable_bounds.min_position_ << " ";
            joint_bounds_hi << variable_bounds.max_position_ << " ";
          }
          RCLCPP_WARN(LOGGER,
                      "Joint '%s' from the starting state is outside bounds by a significant margin: [%s] should be in "
                      "the range [%s], [%s] but the error above the ~%s parameter (currently set to %f)",
                      jmodel->getName().c_str(), joint_values.str().c_str(), joint_bounds_low.str().c_str(),
                      joint_bounds_hi.str().c_str(), BOUNDS_PARAM_NAME.c_str(), bounds_dist_);
        }
      }
    }

    bool solved;
    // if we made any changes, use them
    if (change_req)
    {
      planning_interface::MotionPlanRequest req2 = req;
      robot_state::robotStateToRobotStateMsg(start_state, req2.start_state, false);
      solved = planner(planning_scene, req2, res);
    }
    else
      solved = planner(planning_scene, req, res);

    // re-add the prefix state, if it was constructed
    if (prefix_state && res.trajectory_ && !res.trajectory_->empty())
    {
      // heuristically decide a duration offset for the trajectory (induced by the additional point added as a prefix to
      // the computed trajectory)
      res.trajectory_->setWayPointDurationFromPrevious(
          0, std::min(max_dt_offset_, res.trajectory_->getAverageSegmentDuration()));
      res.trajectory_->addPrefixWayPoint(prefix_state, 0.0);
      // we add a prefix point, so we need to bump any previously added index positions
      for (std::size_t& added_index : added_path_index)
        added_index++;
      added_path_index.push_back(0);
    }

    return solved;
  }

private:
  rclcpp::Node::SharedPtr node_;
  double bounds_dist_;
  double max_dt_offset_;
};

const std::string FixStartStateBounds::BOUNDS_PARAM_NAME = "start_state_max_bounds_error";
const std::string FixStartStateBounds::DT_PARAM_NAME = "start_state_max_dt";
}  // namespace default_planner_request_adapters

CLASS_LOADER_REGISTER_CLASS(default_planner_request_adapters::FixStartStateBounds,
                            planning_request_adapter::PlanningRequestAdapter)
