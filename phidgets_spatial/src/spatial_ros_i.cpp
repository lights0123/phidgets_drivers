/*
 * Copyright (c) 2019, Open Source Robotics Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <functional>
#include <memory>
#include <mutex>

#include <ros/ros.h>
#include <ros/service_server.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/MagneticField.h>
#include <std_msgs/Bool.h>
#include <std_srvs/Empty.h>

#include "phidgets_api/spatial.h"
#include "phidgets_spatial/spatial_ros_i.h"

namespace phidgets {

SpatialRosI::SpatialRosI(ros::NodeHandle nh, ros::NodeHandle nh_private)
    : nh_(nh), nh_private_(nh_private)
{
    ROS_INFO("Starting Phidgets SPATIAL");

    ROS_INFO("Opening spatial");
    int serial_num;
    if (!nh_private_.getParam("serial", serial_num))
    {
        serial_num = -1;  // default open any device
    }
    int hub_port;
    if (!nh_private.getParam("hub_port", hub_port))
    {
        hub_port = 0;  // only used if the device is on a VINT hub_port
    }
    if (!nh_private_.getParam("frame_id", frame_id_))
    {
        // As specified in http://www.ros.org/reps/rep-0145.html
        frame_id_ = "imu_link";
    }

    double linear_acceleration_stdev;
    if (!nh_private_.getParam("linear_acceleration_stdev",
                              linear_acceleration_stdev))
    {
        // 280 ug accelerometer white noise sigma, as per manual
        linear_acceleration_stdev = 280.0 * 1e-6 * G;
    }
    linear_acceleration_variance_ =
        linear_acceleration_stdev * linear_acceleration_stdev;

    double angular_velocity_stdev;
    if (!nh_private_.getParam("angular_velocity_stdev", angular_velocity_stdev))
    {
        // 0.095 deg/s gyroscope white noise sigma, as per manual
        angular_velocity_stdev = 0.095 * (M_PI / 180.0);
    }
    angular_velocity_variance_ =
        angular_velocity_stdev * angular_velocity_stdev;

    double magnetic_field_stdev;
    if (!nh_private_.getParam("magnetic_field_stdev", magnetic_field_stdev))
    {
        // 1.1 milligauss magnetometer white noise sigma, as per manual
        magnetic_field_stdev = 1.1 * 1e-3 * 1e-4;
    }
    magnetic_field_variance_ = magnetic_field_stdev * magnetic_field_stdev;

    int time_resync_ms;
    if (!nh_private_.getParam("time_resynchronization_interval_ms",
                              time_resync_ms))
    {
        time_resync_ms = 5000;
    }
    time_resync_interval_ns_ =
        static_cast<int64_t>(time_resync_ms) * 1000 * 1000;

    int data_interval_ms;
    if (!nh_private.getParam("data_interval_ms", data_interval_ms))
    {
        data_interval_ms = 8;
    }
    data_interval_ns_ = data_interval_ms * 1000 * 1000;

    int cb_delta_epsilon_ms;
    if (!nh_private.getParam("callback_delta_epsilon_ms", cb_delta_epsilon_ms))
    {
        cb_delta_epsilon_ms = 1;
    }
    cb_delta_epsilon_ns_ = cb_delta_epsilon_ms * 1000 * 1000;

    if (cb_delta_epsilon_ms >= data_interval_ms)
    {
        throw std::runtime_error(
            "Callback epsilon is larger than the data interval; this can never "
            "work");
    }

    if (!nh_private.getParam("publish_rate", publish_rate_))
    {
        publish_rate_ = 0;
    }

    if (nh_private.getParam("server_name", server_name_) &&
        nh_private.getParam("server_ip", server_ip_))
    {
        PhidgetNet_addServer(server_name_.c_str(), server_ip_.c_str(), 5661, "",
                             0);

        ROS_INFO("Using phidget server %s at IP %s", server_name_.c_str(),
                 server_ip_.c_str());
    }

    // compass correction params (see
    // http://www.phidgets.com/docs/1044_User_Guide)
    double cc_mag_field;
    double cc_offset0;
    double cc_offset1;
    double cc_offset2;
    double cc_gain0;
    double cc_gain1;
    double cc_gain2;
    double cc_T0;
    double cc_T1;
    double cc_T2;
    double cc_T3;
    double cc_T4;
    double cc_T5;

    bool has_compass_params =
        nh_private_.getParam("cc_mag_field", cc_mag_field) &&
        nh_private_.getParam("cc_offset0", cc_offset0) &&
        nh_private_.getParam("cc_offset1", cc_offset1) &&
        nh_private_.getParam("cc_offset2", cc_offset2) &&
        nh_private_.getParam("cc_gain0", cc_gain0) &&
        nh_private_.getParam("cc_gain1", cc_gain1) &&
        nh_private_.getParam("cc_gain2", cc_gain2) &&
        nh_private_.getParam("cc_t0", cc_T0) &&
        nh_private_.getParam("cc_t1", cc_T1) &&
        nh_private_.getParam("cc_t2", cc_T2) &&
        nh_private_.getParam("cc_t3", cc_T3) &&
        nh_private_.getParam("cc_t4", cc_T4) &&
        nh_private_.getParam("cc_t5", cc_T5);

    ROS_INFO("Connecting to Phidgets Spatial serial %d, hub port %d ...",
             serial_num, hub_port);

    // We take the mutex here and don't unlock until the end of the constructor
    // to prevent a callback from trying to use the publisher before we are
    // finished setting up.
    std::lock_guard<std::mutex> lock(spatial_mutex_);

    try
    {
        spatial_ = std::make_unique<Spatial>(
            serial_num, hub_port, false,
            std::bind(&SpatialRosI::spatialDataCallback, this,
                      std::placeholders::_1, std::placeholders::_2,
                      std::placeholders::_3, std::placeholders::_4),
            std::bind(&SpatialRosI::attachCallback, this),
            std::bind(&SpatialRosI::detachCallback, this));

        ROS_INFO("Connected");

        spatial_->setDataInterval(data_interval_ms);

        imu_pub_ = nh_.advertise<sensor_msgs::Imu>("imu/data_raw", 1);

        cal_publisher_ = nh_.advertise<std_msgs::Bool>("imu/is_calibrated", 5,
                                                       true /* latched */);

        calibrate();

        if (has_compass_params)
        {
            spatial_->setCompassCorrectionParameters(
                cc_mag_field, cc_offset0, cc_offset1, cc_offset2, cc_gain0,
                cc_gain1, cc_gain2, cc_T0, cc_T1, cc_T2, cc_T3, cc_T4, cc_T5);
        } else
        {
            ROS_INFO("No compass correction params found.");
        }
    } catch (const Phidget22Error &err)
    {
        ROS_ERROR("Spatial: %s", err.what());
        throw;
    }

    cal_srv_ = nh_.advertiseService("imu/calibrate",
                                    &SpatialRosI::calibrateService, this);

    magnetic_field_pub_ =
        nh_.advertise<sensor_msgs::MagneticField>("imu/mag", 1);

    if (publish_rate_ > 0)
    {
        timer_ = nh_.createTimer(ros::Duration(1.0 / publish_rate_),
                                 &SpatialRosI::timerCallback, this);
    }
}

bool SpatialRosI::calibrateService(std_srvs::Empty::Request &req,
                                   std_srvs::Empty::Response &res)
{
    (void)req;
    (void)res;
    calibrate();
    return true;
}

void SpatialRosI::calibrate()
{
    ROS_INFO(
        "Calibrating IMU, this takes around 2 seconds to finish. "
        "Make sure that the device is not moved during this time.");
    spatial_->zero();
    // The API call returns directly, so we "enforce" the recommended 2 sec
    // here. See: https://github.com/ros-drivers/phidgets_drivers/issues/40
    ros::Duration(2.).sleep();
    ROS_INFO("Calibrating IMU done.");

    // publish message
    std_msgs::Bool is_calibrated_msg;
    is_calibrated_msg.data = true;
    cal_publisher_.publish(is_calibrated_msg);
}

void SpatialRosI::publishLatest()
{
    std::shared_ptr<sensor_msgs::Imu> msg =
        std::make_shared<sensor_msgs::Imu>();

    std::shared_ptr<sensor_msgs::MagneticField> mag_msg =
        std::make_shared<sensor_msgs::MagneticField>();

    // build covariance matrices
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            if (i == j)
            {
                int idx = j * 3 + i;
                msg->linear_acceleration_covariance[idx] =
                    linear_acceleration_variance_;
                msg->angular_velocity_covariance[idx] =
                    angular_velocity_variance_;
                mag_msg->magnetic_field_covariance[idx] =
                    magnetic_field_variance_;
            }
        }
    }

    // Fill out and send IMU message
    msg->header.frame_id = frame_id_;

    uint64_t imu_diff_in_ns = last_data_timestamp_ns_ - data_time_zero_ns_;
    uint64_t time_in_ns = ros_time_zero_.toNSec() + imu_diff_in_ns;

    if (time_in_ns < last_ros_stamp_ns_)
    {
        ROS_WARN("Time went backwards (%lu < %lu)! Not publishing message.",
                 time_in_ns, last_ros_stamp_ns_);
        return;
    }

    last_ros_stamp_ns_ = time_in_ns;

    ros::Time ros_time = ros::Time().fromNSec(time_in_ns);

    msg->header.stamp = ros_time;

    // set linear acceleration
    msg->linear_acceleration.x = last_accel_x_;
    msg->linear_acceleration.y = last_accel_y_;
    msg->linear_acceleration.z = last_accel_z_;

    // set angular velocities
    msg->angular_velocity.x = last_gyro_x_;
    msg->angular_velocity.y = last_gyro_y_;
    msg->angular_velocity.z = last_gyro_z_;

    imu_pub_.publish(*msg);

    // Fill out and publish magnetic message
    mag_msg->header.frame_id = frame_id_;

    mag_msg->header.stamp = ros_time;

    mag_msg->magnetic_field.x = last_mag_x_;
    mag_msg->magnetic_field.y = last_mag_y_;
    mag_msg->magnetic_field.z = last_mag_z_;

    magnetic_field_pub_.publish(*mag_msg);
}

void SpatialRosI::spatialDataCallback(const double acceleration[3],
                                      const double angular_rate[3],
                                      const double magnetic_field[3],
                                      double timestamp)
{
    // When publishing the message on the ROS network, we want to publish the
    // time that the data was acquired in seconds since the Unix epoch.  The
    // data we have to work with is the time that the callback happened (on the
    // local processor, in Unix epoch seconds), and the timestamp that the
    // IMU gives us on the callback (from the processor on the IMU, in
    // milliseconds since some arbitrary starting point).
    //
    // At a first approximation, we can apply the timestamp from the device to
    // Unix epoch seconds by taking a common starting point on the IMU and the
    // local processor, then applying the delta between this IMU timestamp and
    // the "zero" IMU timestamp to the local processor starting point.
    //
    // There are several complications with the simple scheme above.  The first
    // is finding a proper "zero" point where the IMU timestamp and the local
    // timestamp line up.  Due to potential delays in servicing this process,
    // along with USB delays, the delta timestamp from the IMU and the time when
    // this callback gets called can be wildly different.  Since we want the
    // initial zero for both the IMU and the local time to be in the same time
    // "window", we throw away data at the beginning until we see that the delta
    // callback and delta timestamp are within reasonable bounds of each other.
    //
    // The second complication is that the time on the IMU drifts away from the
    // time on the local processor.  Taking the "zero" time once at the
    // beginning isn't sufficient, and we have to periodically re-synchronize
    // the times given the constraints above.  Because we still have the
    // arbitrary delays present as described above, it can take us several
    // callbacks to successfully synchronize.  We continue publishing data using
    // the old "zero" time until successfully resynchronize, at which point we
    // switch to the new zero point.

    std::lock_guard<std::mutex> lock(spatial_mutex_);

    ros::Time now = ros::Time::now();

    // At the beginning of time, need to initialize last_cb_time for later use;
    // last_cb_time is used to figure out the time between callbacks
    if (last_cb_time_.sec == 0 && last_cb_time_.nsec == 0)
    {
        last_cb_time_ = now;
        return;
    }

    ros::Duration time_since_last_cb = now - last_cb_time_;
    uint64_t this_ts_ns = static_cast<uint64_t>(timestamp * 1000.0 * 1000.0);

    if (synchronize_timestamps_)
    {
        // The only time it's safe to sync time between IMU and ROS Node is when
        // the data that came in is within the data interval that data is
        // expected. It's possible for data to come late because of USB issues
        // or swapping, etc and we don't want to sync with data that was
        // actually published before this time interval, so we wait until we get
        // data that is within the data interval +/- an epsilon since we will
        // have taken some time to process and/or a short delay (maybe USB
        // comms) may have happened
        if (time_since_last_cb.toNSec() >=
                (data_interval_ns_ - cb_delta_epsilon_ns_) &&
            time_since_last_cb.toNSec() <=
                (data_interval_ns_ + cb_delta_epsilon_ns_))
        {
            ros_time_zero_ = now;
            data_time_zero_ns_ = this_ts_ns;
            synchronize_timestamps_ = false;
            can_publish_ = true;
        } else
        {
            ROS_DEBUG(
                "Data not within acceptable window for synchronization: "
                "expected between %ld and %ld, saw %ld",
                data_interval_ns_ - cb_delta_epsilon_ns_,
                data_interval_ns_ + cb_delta_epsilon_ns_,
                time_since_last_cb.toNSec());
        }
    }

    if (can_publish_)  // Cannot publish data until IMU/ROS timestamps have been
                       // synchronized at least once
    {
        // Save off the values
        last_accel_x_ = -acceleration[0] * G;
        last_accel_y_ = -acceleration[1] * G;
        last_accel_z_ = -acceleration[2] * G;

        last_gyro_x_ = angular_rate[0] * (M_PI / 180.0);
        last_gyro_y_ = angular_rate[1] * (M_PI / 180.0);
        last_gyro_z_ = angular_rate[2] * (M_PI / 180.0);

        if (magnetic_field[0] != PUNK_DBL)
        {
            // device reports data in Gauss, multiply by 1e-4 to convert to
            // Tesla
            last_mag_x_ = magnetic_field[0] * 1e-4;
            last_mag_y_ = magnetic_field[1] * 1e-4;
            last_mag_z_ = magnetic_field[2] * 1e-4;
        } else
        {
            // data is PUNK_DBL ("unknown double"), which means the magnetometer
            // did not return valid readings. When publishing at 250 Hz, this
            // will happen in every second message, because the magnetometer can
            // only sample at 125 Hz. It is still important to publish these
            // messages, because a downstream node sometimes uses a
            // TimeSynchronizer to get Imu and Magnetometer nodes.
            double nan = std::numeric_limits<double>::quiet_NaN();

            last_mag_x_ = nan;
            last_mag_y_ = nan;
            last_mag_z_ = nan;
        }
        last_data_timestamp_ns_ = this_ts_ns;

        // Publish if we aren't publishing on a timer
        if (publish_rate_ <= 0)
        {
            publishLatest();
        }
    }

    // Determine if we need to resynchronize - time between IMU and ROS Node can
    // drift, periodically resync to deal with this issue
    ros::Duration diff = now - ros_time_zero_;
    if (time_resync_interval_ns_ > 0 &&
        diff.toNSec() >= time_resync_interval_ns_)
    {
        synchronize_timestamps_ = true;
    }

    last_cb_time_ = now;
}

void SpatialRosI::attachCallback()
{
    ROS_INFO("Phidget Spatial attached.");

    // Set data interval. This is in attachCallback() because it has to be
    // repeated on reattachment.
    spatial_->setDataInterval(data_interval_ns_ / 1000 / 1000);

    // Force resynchronization, because the device time is reset to 0 after
    // reattachment.
    synchronize_timestamps_ = true;
    can_publish_ = false;
    last_cb_time_ = ros::Time(0);
}

void SpatialRosI::detachCallback()
{
    ROS_INFO("Phidget Spatial detached.");
}

void SpatialRosI::timerCallback(const ros::TimerEvent & /* event */)
{
    std::lock_guard<std::mutex> lock(spatial_mutex_);
    if (can_publish_)
    {
        publishLatest();
    }
}

}  // namespace phidgets
