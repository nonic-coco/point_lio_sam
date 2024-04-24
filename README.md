# Point-LIO-SAM
+ This package is a combination of Point-LIO and LIO-SAM for unitree lidar L1.
    + [FAST_LIO_SAM](https://github.com/kahowang/FAST_LIO_SAM): FAST-LIO2 + LIO-SAM


<p align="center">
  <img src="imgs/fast1.png" height="300"/>
  <img src="imgs/sam1.png" height="300"/>
  <br>
  <em>KITTI seq 05 top view - (left): FAST-LIO2 (right): FAST-LIO-SAM</em>
</p>
<p align="center">
  <img src="imgs/fast2.png" width="600"/>
  <img src="imgs/sam2.png" width="600"/>
  <br>
  <em>KITTI seq 05 side view - (top): FAST-LIO2 (bottom): FAST-LIO-SAM</em>
</p>
<p align="center">
  <img src="imgs/traj.png" width="600"/>
  <br>
  <em>KITTI seq 05 trajectories - (blue): FAST-LIO2 (green): FAST-LIO-SAM</em>
</p>

<br>

 
#### Note
+ For better loop-detection and transform calculation, [FAST-LIO-SAM-QN](https://github.com/engcang/FAST-LIO-SAM-QN) is also coded and opened.
    + It adopts [Quatro](https://github.com/url-kaist/Quatro) - fast, accurate and robust global registration which provides great initial guess of transform
    + and [Nano-GICP](https://github.com/vectr-ucla/direct_lidar_odometry) - fast and accurate ICP combining [FastGICP](https://github.com/SMRT-AIST/fast_gicp) + [NanoFLANN](https://github.com/jlblancoc/nanoflann)


<br>

## Dependencies
+ ROS (it comes with `Eigen` and `PCL`)
+ [GTSAM](https://github.com/borglab/gtsam)
    ```shell
    wget -O gtsam.zip https://github.com/borglab/gtsam/archive/refs/tags/4.1.1.zip
    unzip gtsam.zip
    cd gtsam-4.1.1/
    mkdir build && cd build
    cmake -DGTSAM_BUILD_WITH_MARCH_NATIVE=OFF -DGTSAM_USE_SYSTEM_EIGEN=ON ..
    sudo make install -j16
    ```

## How to build and use
+ Get the code and build
    ```shell
    cd ~/your_workspace/src
    git clone https://github.com/engcang/FAST-LIO-SAM --recursive

    cd ..
    catkin build -DCMAKE_BUILD_TYPE=Release
    . devel/setup.bash
    ```
+ Then run (change config files in third_party/`FAST_LIO`)
    ```shell
    roslaunch fast_lio_sam run.launch lidar:=ouster
    roslaunch fast_lio_sam run.launch lidar:=velodyne
    roslaunch fast_lio_sam run.launch lidar:=livox
    ```

<br>

### Structure
+ odom_pcd_cb
    + pub realtime pose in corrected frame
    + keyframe detection -> if keyframe, add to pose graph + save to keyframe queue
    + pose graph optimization with iSAM2
+ loop_timer_func
    + process a saved keyframe
        + detect loop -> if loop, add to pose graph
+ vis_timer_func
    + visualize all **(Note: global map is only visualized once uncheck/check the mapped_pcd in rviz to save comp.)**
