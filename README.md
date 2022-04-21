# UV-SLAM

## Developing !!
Jinwoo Jeon: for Struct-MDC




<center>

| UV-SLAM  ||
| :---: | :---: |
| <img src="https://user-images.githubusercontent.com/42729711/143393647-ec49dab0-b2e0-4c77-831a-03a819125a7f.png">  | <img src="/uploads/131796cc3533ddc8d916ee57e7373944/point_uncertainty.png">

</center>

## Results
- Mapping results for *MH_05_difficult* in the [EuRoC datasets](https://projects.asl.ethz.ch/datasets/doku.php?id=kmavvisualinertialdatasets)
- All mapping results for the EuRoC datasets is available in [here](https://github.com/tp02134/UV-SLAM/blob/main/mapping_result.pdf)

<center>

| [ALVIO](https://link.springer.com/chapter/10.1007/978-981-16-4803-8_19) | [Previous work](https://ieeexplore.ieee.org/document/9560911) | UV-SLAM |
| :---: | :---: | :---: |
| <img src="https://user-images.githubusercontent.com/42729711/143398005-afce16e2-c3dc-4c3b-af6e-adade9a45d56.png">  | <img src="https://user-images.githubusercontent.com/42729711/143397993-edb67494-b00c-47e1-8591-532cf0c4cc46.png">  |  <img src="https://user-images.githubusercontent.com/42729711/143398028-9cf349f8-510e-4709-9859-4ff752b47f13.png">  |

</center>

## Installation
### 1. Prerequisites
**1.1 Ubuntu**

**1.3 ROS**

**1.2 Ceres Solver**

### 2. Build
Clone the repository and ```catkin_make```:
```
cd ~/<your_workspace>/src
git clone https://github.com/url-kaist/UV-SLAM.git
cd ../
catkin_make
source ~/<your_workspace>/devel/setup.bash
```
### 3. Trouble shooting
If you have installed [VINS-mono](https://github.com/HKUST-Aerial-Robotics/VINS-Mono) before, remove the common packages.

example: ```benchmark_publisher```, ```camera_model```, etc

## Run on EuRoC datasets
Download [EuRoC MAV Dataset](http://projects.asl.ethz.ch/datasets/doku.php?id=kmavvisualinertialdatasets).

Open three terminals and launch the vins_estimator, rviz, and play the bag file, respectively.
```
roslaunch uv_slam euroc.launch
roslaunch uv_slam vins_rviz.launch
rosbag play <dataset>
```

## Citation
If you use the algorithm in an academic context, please cite the following publication:
```

```

## Acknowledgements
We use [VINS-Mono](https://github.com/HKUST-Aerial-Robotics/VINS-Mono) as our baseline code. Thanks Dr. Qin Tong, Prof. Shen etc very much.
For vanishing point extraction, we use [J-linkage](http://www.diegm.uniud.it/fusiello/demo/jlk/) or [2-line exhaustive searching method](https://github.com/xiaohulugo/VanishingPointDetection).

## Licence
The source code is released under [GPLv3](http://www.gnu.org/licenses/) license.
We are still working on improving the code reliability.
For any technical issues, please contact Hyunjun Lim (tp02134@kaist.ac.kr).
