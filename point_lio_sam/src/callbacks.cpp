#include "main.h"


void FAST_LIO_SAM_CLASS::odom_pcd_cb(const nav_msgs::OdometryConstPtr &odom_msg, const sensor_msgs::PointCloud2ConstPtr &pcd_msg)
{
  Eigen::Matrix4d last_odom_tf_;
  last_odom_tf_ = m_current_frame.pose_eig; //to calculate delta
  m_current_frame = pose_pcd(*odom_msg, *pcd_msg, m_current_keyframe_idx); //to be checked if keyframe or not

  // Debug print
  // std::cerr << "Odom: " << m_current_frame.pose_eig << endl;
  // std::cerr << "PCD: " << m_current_frame.pcd.size() << endl;
  if (!m_init) //// init only once
  {
    //others
    m_keyframes.push_back(m_current_frame);
    update_vis_vars(m_current_frame);
    m_corrected_current_pcd_pub.publish(pcl_to_pcl_ros(m_current_frame.pcd, m_map_frame));
    //graph
    gtsam::noiseModel::Diagonal::shared_ptr prior_noise_ = gtsam::noiseModel::Diagonal::Variances((gtsam::Vector(6) << 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4).finished()); // rad*rad, meter*meter
    m_gtsam_graph.add(gtsam::PriorFactor<gtsam::Pose3>(0, pose_eig_to_gtsam_pose(m_current_frame.pose_eig), prior_noise_));
    m_init_esti.insert(m_current_keyframe_idx, pose_eig_to_gtsam_pose(m_current_frame.pose_eig));
    {
      lock_guard<mutex> lock(m_realtime_pose_mutex);
      m_odom_delta = m_odom_delta * last_odom_tf_.inverse() * m_current_frame.pose_eig;
      m_current_frame.pose_corrected_eig = m_last_corrected_pose * m_odom_delta;
      geometry_msgs::PoseStamped current_pose_stamped_ = pose_eig_to_pose_stamped(m_current_frame.pose_corrected_eig, m_map_frame);
      m_realtime_pose_pub.publish(current_pose_stamped_);
      tf::Transform transform_;
      transform_.setOrigin(tf::Vector3(current_pose_stamped_.pose.position.x, current_pose_stamped_.pose.position.y, current_pose_stamped_.pose.position.z));
      transform_.setRotation(tf::Quaternion(current_pose_stamped_.pose.orientation.x, current_pose_stamped_.pose.orientation.y, current_pose_stamped_.pose.orientation.z, current_pose_stamped_.pose.orientation.w));
      m_broadcaster.sendTransform(tf::StampedTransform(transform_, ros::Time::now(), m_map_frame, "robot"));
    }
    m_current_keyframe_idx++;
    m_init = true;
  }
  else
  {
    //// 1. realtime pose = last corrected odom * delta (last -> current)
    high_resolution_clock::time_point t1_ = high_resolution_clock::now();
    {
      lock_guard<mutex> lock(m_realtime_pose_mutex);
      m_odom_delta = m_odom_delta * last_odom_tf_.inverse() * m_current_frame.pose_eig;
      m_current_frame.pose_corrected_eig = m_last_corrected_pose * m_odom_delta;
      geometry_msgs::PoseStamped current_pose_stamped_ = pose_eig_to_pose_stamped(m_current_frame.pose_corrected_eig, m_map_frame);
      m_realtime_pose_pub.publish(current_pose_stamped_);
      tf::Transform transform_;
      transform_.setOrigin(tf::Vector3(current_pose_stamped_.pose.position.x, current_pose_stamped_.pose.position.y, current_pose_stamped_.pose.position.z));
      transform_.setRotation(tf::Quaternion(current_pose_stamped_.pose.orientation.x, current_pose_stamped_.pose.orientation.y, current_pose_stamped_.pose.orientation.z, current_pose_stamped_.pose.orientation.w));
      m_broadcaster.sendTransform(tf::StampedTransform(transform_, ros::Time::now(), m_map_frame, "robot"));
    }
    // pub current scan in corrected pose frame
    m_corrected_current_pcd_pub.publish(pcl_to_pcl_ros(tf_pcd(m_current_frame.pcd, m_current_frame.pose_corrected_eig), m_map_frame));

    //// 2. check if keyframe
    high_resolution_clock::time_point t2_ = high_resolution_clock::now();
    if (check_if_keyframe(m_current_frame, m_keyframes.back()))
    {
      // 2-2. if so, save
      {
        lock_guard<mutex> lock(m_keyframes_mutex);
        m_keyframes.push_back(m_current_frame);
        m_not_processed_keyframe = m_current_frame; //to check loop in another thread
      }
      // 2-3. if so, add to graph
      gtsam::noiseModel::Diagonal::shared_ptr odom_noise_ = gtsam::noiseModel::Diagonal::Variances((gtsam::Vector(6) << 1e-4, 1e-4, 1e-4, 1e-2, 1e-2, 1e-2).finished());
      gtsam::Pose3 pose_from_ = pose_eig_to_gtsam_pose(m_keyframes[m_current_keyframe_idx-1].pose_corrected_eig);
      gtsam::Pose3 pose_to_ = pose_eig_to_gtsam_pose(m_current_frame.pose_corrected_eig);
      {
        lock_guard<mutex> lock(m_graph_mutex);
        m_gtsam_graph.add(gtsam::BetweenFactor<gtsam::Pose3>(m_current_keyframe_idx-1, m_current_keyframe_idx, pose_from_.between(pose_to_), odom_noise_));
        m_init_esti.insert(m_current_keyframe_idx, pose_to_);
      }
      m_current_keyframe_idx++;

      //// 3. vis
      high_resolution_clock::time_point t3_ = high_resolution_clock::now();
      {
        lock_guard<mutex> lock(m_vis_mutex);
        update_vis_vars(m_current_frame);
      }

      //// 4. optimize with graph
      high_resolution_clock::time_point t4_ = high_resolution_clock::now();
      // m_corrected_esti = gtsam::LevenbergMarquardtOptimizer(m_gtsam_graph, m_init_esti).optimize(); // cf. isam.update vs values.LM.optimize
      {
        lock_guard<mutex> lock(m_graph_mutex);
        m_isam_handler->update(m_gtsam_graph, m_init_esti);
        m_isam_handler->update();
        if (m_loop_added_flag) //https://github.com/TixiaoShan/LIO-SAM/issues/5#issuecomment-653752936
        {
          m_isam_handler->update();
          m_isam_handler->update();
          m_isam_handler->update();
        }
        m_gtsam_graph.resize(0);
        m_init_esti.clear();
      }

      //// 5. handle corrected results
      // get corrected poses and reset odom delta (for realtime pose pub)
      high_resolution_clock::time_point t5_ = high_resolution_clock::now();
      {
        lock_guard<mutex> lock(m_realtime_pose_mutex);
        m_corrected_esti = m_isam_handler->calculateEstimate();
        m_last_corrected_pose = gtsam_pose_to_pose_eig(m_corrected_esti.at<gtsam::Pose3>(m_corrected_esti.size()-1));
        m_odom_delta = Eigen::Matrix4d::Identity();
      }
      // correct poses in keyframes
      if (m_loop_added_flag)
      {
        lock_guard<mutex> lock(m_keyframes_mutex);
        for (int i = 0; i < m_corrected_esti.size(); ++i)
        {
          m_keyframes[i].pose_corrected_eig = gtsam_pose_to_pose_eig(m_corrected_esti.at<gtsam::Pose3>(i));
        }
        m_loop_added_flag = false;
      }
      high_resolution_clock::time_point t6_ = high_resolution_clock::now();

      ROS_INFO("real: %.1f, key_add: %.1f, vis: %.1f, opt: %.1f, res: %.1f, tot: %.1fms", 
        duration_cast<microseconds>(t2_-t1_).count()/1e3, duration_cast<microseconds>(t3_-t2_).count()/1e3,
        duration_cast<microseconds>(t4_-t3_).count()/1e3, duration_cast<microseconds>(t5_-t4_).count()/1e3,
        duration_cast<microseconds>(t6_-t5_).count()/1e3, duration_cast<microseconds>(t6_-t1_).count()/1e3);
    }
  }
  return;
}

void FAST_LIO_SAM_CLASS::loop_timer_func(const ros::TimerEvent& event)
{
  if (!m_init) return;

  //// 1. copy keyframes and not processed keyframes
  high_resolution_clock::time_point t1_ = high_resolution_clock::now();
  pose_pcd not_proc_key_copy_;
  vector<pose_pcd> keyframes_copy_;
  {
    lock_guard<mutex> lock(m_keyframes_mutex);
    keyframes_copy_ = m_keyframes;
    not_proc_key_copy_ = m_not_processed_keyframe;
    m_not_processed_keyframe.processed = true;
  }
  if (not_proc_key_copy_.processed) return; //already processed

  //// 2. detect loop and add to graph
  high_resolution_clock::time_point t2_ = high_resolution_clock::now();
  bool if_loop_occured_ = false;
  // from not_proc_key_copy_ keyframe to old keyframes in threshold radius, get the closest keyframe
  int closest_keyframe_idx_ = get_closest_keyframe_idx(not_proc_key_copy_, keyframes_copy_);
  if (closest_keyframe_idx_ >= 0) //if exists
  {
    // ICP to check loop (from front_keyframe to closest keyframe's neighbor)
    icp_key_to_subkeys(not_proc_key_copy_, closest_keyframe_idx_, keyframes_copy_);
    double score_ = m_icp.getFitnessScore();
    cout << score_ << endl;
    // if matchness score is lower than threshold, (lower is better)
    if(m_icp.hasConverged() && score_ < m_icp_score_thr) // add loop factor
    {
      Eigen::Matrix4d pose_between_eig_ = m_icp.getFinalTransformation().cast<double>();
      gtsam::Pose3 pose_from_ = pose_eig_to_gtsam_pose(pose_between_eig_ * not_proc_key_copy_.pose_corrected_eig);
      gtsam::Pose3 pose_to_ = pose_eig_to_gtsam_pose(keyframes_copy_[closest_keyframe_idx_].pose_corrected_eig);
      gtsam::noiseModel::Diagonal::shared_ptr loop_noise_ = gtsam::noiseModel::Diagonal::Variances((gtsam::Vector(6) << score_, score_, score_, score_, score_, score_).finished());
      {
        lock_guard<mutex> lock(m_graph_mutex);
        m_gtsam_graph.add(gtsam::BetweenFactor<gtsam::Pose3>(not_proc_key_copy_.idx, closest_keyframe_idx_, pose_from_.between(pose_to_), loop_noise_));
      }
      m_loop_idx_pairs.push_back({not_proc_key_copy_.idx, closest_keyframe_idx_}); //for vis
      if_loop_occured_ = true;
    }
  }
  high_resolution_clock::time_point t3_ = high_resolution_clock::now();

  if (if_loop_occured_)
  {
    m_loop_added_flag_vis = true;
    m_loop_added_flag = true;
  }

  ROS_INFO("copy: %.1f, loop: %.1f", 
          duration_cast<microseconds>(t2_-t1_).count()/1e3, duration_cast<microseconds>(t3_-t2_).count()/1e3);

  return;
}

void FAST_LIO_SAM_CLASS::vis_timer_func(const ros::TimerEvent& event)
{
  if (!m_init) return;

  high_resolution_clock::time_point tv1_ = high_resolution_clock::now();
  //// 1. if loop closed, correct vis data
  if (m_loop_added_flag_vis) 
  {
    // copy and ready
    gtsam::Values corrected_esti_copy_;
    pcl::PointCloud<pcl::PointXYZ> corrected_odoms_;
    nav_msgs::Path corrected_path_;
    {
      lock_guard<mutex> lock(m_realtime_pose_mutex);
      corrected_esti_copy_ = m_corrected_esti;
    }
    // correct pose and path
    for (int i = 0; i < corrected_esti_copy_.size(); ++i)
    {
      gtsam::Pose3 pose_ = corrected_esti_copy_.at<gtsam::Pose3>(i);
      corrected_odoms_.points.emplace_back(pose_.translation().x(), pose_.translation().y(), pose_.translation().z());
      corrected_path_.poses.push_back(gtsam_pose_to_pose_stamped(pose_, m_map_frame));
    }
    // update vis of loop constraints
    if (!m_loop_idx_pairs.empty())
    {
      m_loop_detection_pub.publish(get_loop_markers(corrected_esti_copy_));
    }
    // update with corrected data
    {
      lock_guard<mutex> lock(m_vis_mutex);
      m_corrected_odoms = corrected_odoms_;
      m_corrected_path.poses = corrected_path_.poses;
    }
    m_loop_added_flag_vis = false;
  }

  //// 2. publish odoms, paths
  {
    lock_guard<mutex> lock(m_vis_mutex);
    m_odom_pub.publish(pcl_to_pcl_ros(m_odoms, m_map_frame));
    m_path_pub.publish(m_odom_path);
    m_corrected_odom_pub.publish(pcl_to_pcl_ros(m_corrected_odoms, m_map_frame));
    m_corrected_path_pub.publish(m_corrected_path);
  }

  //// 3. global map
  if (m_global_map_vis_switch && m_corrected_pcd_map_pub.getNumSubscribers() > 0) //save time, only once
  {
    pcl::PointCloud<pcl::PointXYZI> corrected_map_;
    {
      lock_guard<mutex> lock(m_keyframes_mutex);
      for (int i = 0; i < m_keyframes.size(); ++i)
      {
        corrected_map_ += tf_pcd(m_keyframes[i].pcd, m_keyframes[i].pose_corrected_eig);
      }
    }
    voxelize_pcd(m_voxelgrid_vis, corrected_map_);
    m_corrected_pcd_map_pub.publish(pcl_to_pcl_ros(corrected_map_, m_map_frame));
    m_global_map_vis_switch = false;
  }
  if (!m_global_map_vis_switch && m_corrected_pcd_map_pub.getNumSubscribers() == 0)
  {
    m_global_map_vis_switch = true;      
  }
  high_resolution_clock::time_point tv2_ = high_resolution_clock::now();
  ROS_INFO("vis: %.1fms", duration_cast<microseconds>(tv2_-tv1_).count()/1e3);
  return;
}
