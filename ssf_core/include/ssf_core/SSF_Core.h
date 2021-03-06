/*

Copyright (c) 2010, Stephan Weiss, ASL, ETH Zurich, Switzerland
You can contact the author at <stephan dot weiss at ieee dot org>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
* Neither the name of ETHZ-ASL nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETHZ-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef SSF_CORE_H_
#define SSF_CORE_H_


#include <Eigen/Eigen>

#include <ros/ros.h>
#include <dynamic_reconfigure/server.h>
#include <ssf_core/SSF_CoreConfig.h>

#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>

// message includes
#include <sensor_fusion_comm/DoubleArrayStamped.h>
#include <sensor_fusion_comm/ExtState.h>
#include <sensor_fusion_comm/ExtEkf.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/MagneticField.h>
#include <ssf_core/visensor_imu.h>

#include <vector>
#include <ssf_core/state.h>

#include <tf2_ros/transform_broadcaster.h>
#include <tf2_eigen/tf2_eigen.h>
// #include <tf_conversions/tf_eigen.h>

#include <iostream>
#include <cmath>

#include <mutex>

#define N_STATE_BUFFER 256	///< size of unsigned char, do not change!
#define HLI_EKF_STATE_SIZE 16 	///< number of states exchanged with external propagation. Here: p,v,q,bw,bw=16

namespace ssf_core{

typedef dynamic_reconfigure::Server<ssf_core::SSF_CoreConfig> ReconfigureServer;

struct ImuInputsCache{
	int seq;
	Eigen::Matrix<double,3,1> w_m_;         ///< angular velocity from IMU
	Eigen::Matrix<double,3,1> a_m_;         ///< acceleration from IMU
	//Eigen::Quaternion<double> m_m_;         ///< magnetometer readings 
	Eigen::Matrix<double,3,1> m_m_;
	Eigen::Quaternion<double> q_m_;
};

enum ClosestStateStatus{
	TOO_OLD,
	TOO_EARLY,
	FOUND
};

class SSF_Core
{

public:
	typedef Eigen::Matrix<double, N_STATE, 1> ErrorState;
	typedef Eigen::Matrix<double, N_STATE, N_STATE> ErrorStateCov;

	/// big init routine
	void initialize(const Eigen::Matrix<double, 3, 1> & p, const Eigen::Matrix<double, 3, 1> & v,
									const Eigen::Quaternion<double> & q, const Eigen::Matrix<double, 3, 1> & b_w,
									const Eigen::Matrix<double, 3, 1> & b_a, const double & L, const Eigen::Quaternion<double> & q_wv,
									const Eigen::Matrix<double, N_STATE, N_STATE> & P, const Eigen::Matrix<double, 3, 1> & w_m,
									const Eigen::Matrix<double, 3, 1> & a_m, const Eigen::Matrix<double, 3, 1> & m_m,
									const Eigen::Matrix<double, 3, 1> & g,
									const Eigen::Quaternion<double> & q_ci, const Eigen::Matrix<double, 3, 1> & p_ci);

	/// retreive all state information at time t. Used to build H, residual and noise matrix by update sensors
	ClosestStateStatus getClosestState(State*& timestate, ros::Time tstamp, double delay, unsigned char &idx);

	/// get all state information at a given index in the ringbuffer
	//bool getStateAtIdx(State* timestate, unsigned char idx);

	bool isInitFilter(){return config_.init_filter;}
	double getInitScale(){return config_.scale_init;}

	int getNumberofState(){return idx_state_;};

	ros::Time getGlobalStart(){return global_start_;}

	void setGlobalStart(const ros::Time global_start)
	{
		if (lastImuInputsTime_.isZero())
		{
			std::cerr << "ERROR: global_start_ cannot be set before first IMU inputs coming in." << std::endl;
			return;
		}

		if (global_start_.isZero())
		{
			if (idx_state_ != 1)
			{
				std::cerr << "ERROR: global_start_ should be set only after State zero initilisation" << std::endl;
				exit(-1);
				return;
			}
			global_start_ = global_start;
			StateBuffer_[idx_state_ - 1].time_ = global_start_.toSec();
		}else{
			std::cerr << "ERROR: global_start_ has already been set previously" << std::endl;
		}
		 
	}

	ros::Time getLastImuInputsTime(){return lastImuInputsTime_;}

	bool getImuInputsCache(struct ImuInputsCache*& ref, int& size )
	{
		ref = imuInputsCache; // pointer assignment
		size = imuInputsCache_size;

		return isImuCacheReady;
	}

	State getCurrentState(unsigned char& idx){idx = idx_state_; return StateBuffer_[idx_state_];}

	SSF_Core();
	~SSF_Core();

private:

	std::mutex core_mutex;

	ros::WallTimer check_synced_timer_;
	int imu_received_, mag_received_, all_received_;
	static void increment(int* value)
	{
		++(*value);
	}

	message_filters::Subscriber<sensor_msgs::Imu> subImu_; ///< subscriber to IMU readings
	message_filters::Subscriber<sensor_msgs::MagneticField> subMag_;
	/// IMPORTANT! message_filters::Subscriber MUST COME BEFORE message_filters::TimeSynchronizer

	// typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Imu, sensor_msgs::MagneticField> ApproximatePolicy;
	// typedef message_filters::Synchronizer<ApproximatePolicy> ApproximateSync;
	//ApproximateSync approximate_sync_;
	typedef message_filters::sync_policies::ExactTime<sensor_msgs::Imu, sensor_msgs::MagneticField> ExactPolicy;
	typedef message_filters::Synchronizer<ExactPolicy> ExactSync;
	ExactSync exact_sync_;
	
	//message_filters::TimeSynchronizer<sensor_msgs::Imu, sensor_msgs::MagneticField> sync;
	


	const static int nFullState_ = 28; ///< complete state
	const static int nBuff_ = 30; ///< buffer size for median q_vw
	const static int nMaxCorr_ = 50; ///< number of IMU measurements buffered for time correction actions
	const static int QualityThres_ = 1e3;

	Eigen::Matrix<double, N_STATE, N_STATE> Fd_; ///< discrete state propagation matrix
	Eigen::Matrix<double, N_STATE, N_STATE> Qd_; ///< discrete propagation noise matrix

	/// state variables
	State StateBuffer_[N_STATE_BUFFER]; ///< EKF ringbuffer containing pretty much all info needed at time t
	unsigned char idx_state_; ///< pointer to state buffer at most recent state
	unsigned char idx_P_; ///< pointer to state buffer at P latest propagated
	unsigned char idx_time_; ///< pointer to state buffer at a specific time

	Eigen::Matrix<double, 3, 1> g_; ///< gravity vector
	Eigen::Quaternion<double> initial_q_;

	/// vision-world drift watch dog to determine fuzzy tracking
	int qvw_inittimer_;
	Eigen::Matrix<double, nBuff_, 4> qbuff_;

	/// correction from EKF update
	Eigen::Matrix<double, N_STATE, 1> correction_;

	/// dynamic reconfigure config
	ssf_core::SSF_CoreConfig config_;

	Eigen::Matrix<double, 3, 3> R_IW_; ///< Rot IMU->World
	Eigen::Matrix<double, 3, 3> R_CI_; ///< Rot Camera->IMU
	Eigen::Matrix<double, 3, 3> R_WV_; ///< Rot World->Vision

	ros::Time global_start_;
	ros::Time lastImuInputsTime_;
	
	const static int imuInputsCache_size = 64;
	struct ImuInputsCache imuInputsCache[imuInputsCache_size];
	bool isImuCacheReady;

	bool _is_pose_of_camera_not_imu;

	//bool predictionMade_;

	/// enables internal state predictions for log replay
	/**
	 * used to determine if internal states get overwritten by the external
	 * state prediction (online) or internal state prediction is performed
	 * for log replay, when the external prediction is not available.
	 */
	//bool data_playback_;

	

	enum
	{
		NO_UP, GOOD_UP, FUZZY_UP
	};

	ros::Publisher pubState_; ///< publishes all states of the filter
	sensor_fusion_comm::DoubleArrayStamped msgState_;

	ros::Publisher pubPose_; ///< publishes 6DoF pose output
	geometry_msgs::PoseWithCovarianceStamped msgPose_;

	ros::Publisher pubPoseCorrected_; ///< HM: publishes 6DoF pose output, after each applyCorrection
	geometry_msgs::PoseWithCovarianceStamped msgPoseCorrected_;

	ros::Publisher pubIntPose_;
	geometry_msgs::PoseWithCovarianceStamped msgIntPose_;

	tf2_ros::TransformBroadcaster tf_broadcaster_;

	// ros::Publisher pubPoseCrtl_; ///< publishes 6DoF pose including velocity output
	// sensor_fusion_comm::ExtState msgPoseCtrl_;

	//ros::Publisher pubCorrect_; ///< publishes corrections for external state propagation
	//sensor_fusion_comm::ExtEkf msgCorrect_;

	//ros::Subscriber subState_; ///< subscriber to external state propagation


	//sensor_fusion_comm::ExtEkf hl_state_buf_; ///< buffer to store external propagation data

	// dynamic reconfigure
	ReconfigureServer *reconfServer_; // hm: it is this - dynamic_reconfigure::Server<ssf_core::SSF_CoreConfig>
	typedef boost::function<void(ssf_core::SSF_CoreConfig& config, uint32_t level)> CallbackType;
	std::vector<CallbackType> callbacks_;

	/// propagates the state with given dt
	void propagateState(const double dt);

	/// propagets the error state covariance
	void predictProcessCovariance(const double dt);

	/// applies the correction
	bool applyCorrection(unsigned char idx_delaystate, const ErrorState & res_delayed, double fuzzythres, std_msgs::Header msg_header);

	/// propagate covariance to a given index in the ringbuffer
	void propPToIdx(unsigned char idx);

	/// internal state propagation
	/**
	 * This function gets called on incoming imu messages an then performs
	 * the state prediction internally. Only use this OR stateCallback by
	 * remapping the topics accordingly.
	 * \sa{stateCallback}
	 */
	// void imuCallbackHandler(const sensor_msgs::ImuConstPtr & msg);
	void imuCallback(const sensor_msgs::ImuConstPtr & msg, const sensor_msgs::MagneticFieldConstPtr & msg_mag);


	/// external state propagation
	/**
	 * This function gets called when state prediction is performed externally,
	 * e.g. by asctec_mav_framework. Msg has to be the latest predicted state.
	 * Only use this OR imuCallback by remapping the topics accordingly.
	 * \sa{imuCallback}
	 */
	//void stateCallback(const sensor_fusion_comm::ExtEkfConstPtr & msg);

	/// gets called by dynamic reconfigure and calls all registered callbacks in callbacks_
	void Config(ssf_core::SSF_CoreConfig &config, uint32_t level);

	/// handles the dynamic reconfigure for ssf_core
	void DynConfig(ssf_core::SSF_CoreConfig &config, uint32_t level);

	/// computes the median of a given vector
	double getMedian(const Eigen::Matrix<double, nBuff_, 1> & data);

	void checkInputsSynchronized()
	{
		int threshold = 3 * all_received_;
		if (imu_received_ >= threshold || mag_received_ >= threshold ) {
			ROS_WARN("[ssf_core] Low number of synchronized imu/magnetometer tuples received.\n"
								"imu received:       %d (topic '%s')\n"
								"magnetometer received:      %d (topic '%s')\n"
								"Synchronized tuples: %d\n"
								"Possible issues:\n"
								"\t* stereo_image_proc is not running.\n"
								"\t  Does `rosnode info %s` show any connections?\n"
								"\t* The cameras are not synchronized.\n"
								"\t  Try restarting the node with parameter _approximate_sync:=True\n"
								"\t* The network is too slow. One or more images are dropped from each tuple.\n"
								"\t  Try restarting the node",
								imu_received_, subImu_.getTopic().c_str(),
								mag_received_, subMag_.getTopic().c_str(),
								all_received_, ros::this_node::getName().c_str());
		}
	}

public:

	void mutexLock(){core_mutex.lock();}

	void mutexUnlock(){core_mutex.unlock();}
	// some header implementations

	/// main update routine called by a given sensor
	template<class H_type, class Res_type, class R_type>
		bool applyMeasurement(unsigned char idx_delaystate, const Eigen::MatrixBase<H_type>& H_delayed,
			const Eigen::MatrixBase<Res_type> & res_delayed, const Eigen::MatrixBase<R_type>& R_delayed,
			std_msgs::Header msg_header, double fuzzythres = 0.1)
		{
			EIGEN_STATIC_ASSERT_FIXED_SIZE(H_type);
			EIGEN_STATIC_ASSERT_FIXED_SIZE(R_type);

			// get measurements
			if (lastImuInputsTime_.isZero())
			{
				ROS_WARN("Measurement received but no IMU inputs are available yet.");
				exit(-1);
				return false;
			}

			// make sure we have correctly propagated cov until idx_delaystate
			propPToIdx(idx_delaystate);

			R_type S;
			Eigen::Matrix<double, N_STATE, R_type::RowsAtCompileTime> K;
			ErrorStateCov & P = StateBuffer_[idx_delaystate].P_;

			std::cout << "P before update: " << std::endl << P.diagonal().transpose() << std::endl;

			S = H_delayed * StateBuffer_[idx_delaystate].P_ * H_delayed.transpose() + R_delayed;
			K = P * H_delayed.transpose() * S.inverse();

			std::cout << "gain K.diagonal():" << std::endl << K.diagonal().transpose() << std::endl;

			correction_ = K * res_delayed;
			const ErrorStateCov KH = (ErrorStateCov::Identity() - K * H_delayed);
			P = KH * P * KH.transpose() + K * R_delayed * K.transpose();

			// make sure P stays symmetric
			P = 0.5 * (P + P.transpose());

			std::cout << "P after update: " << std::endl << P.diagonal().transpose() << std::endl;

			return applyCorrection(idx_delaystate, correction_, fuzzythres, msg_header);
		}

	/// registers dynamic reconfigure callbacks
	template<class T>
		void registerCallback(void(T::*cb_func)(ssf_core::SSF_CoreConfig& config, uint32_t level), T* p_obj)
		{
			callbacks_.push_back(boost::bind(cb_func, p_obj, _1, _2));
		}

	void broadcast_ci_transformation(const unsigned char idx, const ros::Time& timestamp, bool gotMeasurement = false);
	void broadcast_iw_transformation(const unsigned char idx, const ros::Time& timestamp, bool gotMeasurement = false);
};

};// end namespace

#endif /* SSF_CORE_H_ */
