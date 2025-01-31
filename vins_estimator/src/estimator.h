#pragma once

#include "parameters.h"
#include "feature_manager.h"
#include "utility/utility.h"
#include "utility/tic_toc.h"
#include "initial/solve_5pts.h"
#include "initial/initial_sfm.h"
#include "initial/initial_alignment.h"
#include "initial/initial_ex_rotation.h"
#include <std_msgs/Header.h>
#include <std_msgs/Float32.h>
#include <sensor_msgs/Image.h>
#include <geometry_msgs/TransformStamped.h>

#include <ceres/ceres.h>
#include "factor/imu_factor.h"
#include "factor/pose_local_parameterization.h"
#include "factor/projection_factor.h"
#include "factor/projection_factor_virtual.h"
#include "factor/projection_td_factor.h"
#include "factor/marginalization_factor.h"
#include "factor/line_projection_factor.h"
#include "factor/vp_projection_factor.h"
#include <unordered_map>
#include <queue>
#include <opencv2/core/eigen.hpp>
#include <Eigen/Dense>


#include "CDT.h"



class Estimator
{
  public:
    Estimator();

    void setParameter();

    // interface
    void processIMU(double t, const Vector3d &linear_acceleration, const Vector3d &angular_velocity);
    // todo
    void processImage(const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image,
                      const map<int, vector<Eigen::Matrix<double, 15, 1>>> &image_line,
                      const std_msgs::Header &header,
                      const Mat latest_image,
                      const Mat latest_depth);
    void processImage(const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image,
                      const map<int, vector<Eigen::Matrix<double, 15, 1>>> &image_line,
                      const std_msgs::Header &header,
                      const Mat latest_image,
                      const geometry_msgs::TransformStamped latestGT_msg);
    void setReloFrame(double _frame_stamp, int _frame_index, vector<Vector3d> &_match_points, Vector3d _relo_t, Matrix3d _relo_r);

    // internal
    void clearState();
    bool initialStructure();
    bool visualInitialAlign();
    bool relativePose(Matrix3d &relative_R, Vector3d &relative_T, int &l);
    bool relativePoseForLine(Matrix3d &relative_R, Vector3d &relative_T, int &l);
    void slideWindow();
    void solveOdometry(double header_t);
    void slideWindowNew();
    void slideWindowOld();
    void optimization();
    void vector2double();
    void double2vector();
    bool failureDetection();


    enum SolverFlag
    {
        INITIAL,
        NON_LINEAR
    };

    enum MarginalizationFlag
    {
        MARGIN_OLD = 0,
        MARGIN_SECOND_NEW = 1
    };

    int DEPTH_SCALE = 256;

    SolverFlag solver_flag;
    MarginalizationFlag  marginalization_flag;
    Vector3d g;
    MatrixXd Ap[2], backup_A;
    VectorXd bp[2], backup_b;

    Matrix3d ric[NUM_OF_CAM];
    Vector3d tic[NUM_OF_CAM];

    Vector3d Ps[(WINDOW_SIZE + 1)];
    Vector3d Vs[(WINDOW_SIZE + 1)];
    Matrix3d Rs[(WINDOW_SIZE + 1)];
    Vector3d Bas[(WINDOW_SIZE + 1)];
    Vector3d Bgs[(WINDOW_SIZE + 1)];
    double td;

    Quaterniond q_gt;
    Vector3d t_gt;

    Matrix3d back_R0, last_R, last_R0;
    Vector3d back_P0, last_P, last_P0;
    std_msgs::Header Headers[(WINDOW_SIZE + 1)];

    IntegrationBase *pre_integrations[(WINDOW_SIZE + 1)];
    Vector3d acc_0, gyr_0;

    vector<double> dt_buf[(WINDOW_SIZE + 1)];
    vector<Vector3d> linear_acceleration_buf[(WINDOW_SIZE + 1)];
    vector<Vector3d> angular_velocity_buf[(WINDOW_SIZE + 1)];

    int frame_count;
    int sum_of_outlier, sum_of_back, sum_of_front, sum_of_invalid;

    FeatureManager f_manager;
    MotionEstimator m_estimator;
    InitialEXRotation initial_ex_rotation;

    bool first_imu;
    bool is_valid, is_key;
    bool failure_occur;

    vector<Vector3d> point_cloud;
    vector<Vector3d> margin_cloud;
    vector<Vector3d> key_poses;
    double initial_timestamp;

    vector<pair<Vector3d, Vector3d>> cdt_lines_vis;
    vector<Vector3d> cdt_points;



    double para_Pose[WINDOW_SIZE + 1][SIZE_POSE];
    double para_SpeedBias[WINDOW_SIZE + 1][SIZE_SPEEDBIAS];
    double para_Feature[NUM_OF_F][SIZE_FEATURE]; // INVERSE DEPTH
    double para_Depth[NUM_OF_F][SIZE_FEATURE]; // DEPTH
    double para_Ex_Pose[NUM_OF_CAM][SIZE_POSE];
    double para_Retrive_Pose[SIZE_POSE];
    double para_Td[1][1];
    double para_Tr[1][1];
    double para_Ortho_plucker[NUM_OF_LF][SIZE_LINE_FEATURE];

    int loop_window_index;

    MarginalizationInfo *last_marginalization_info;
    vector<double *> last_marginalization_parameter_blocks;

    map<double, ImageFrame> all_image_frame;
    IntegrationBase *tmp_pre_integration;

    //relocalization variable
    bool relocalization_info;
    double relo_frame_stamp;
    double relo_frame_index;
    int relo_frame_local_index;
    vector<Vector3d> match_points;
    double relo_Pose[SIZE_POSE];
    Matrix3d drift_correct_r;
    Vector3d drift_correct_t;
    Vector3d prev_relo_t;
    Matrix3d prev_relo_r;
    Vector3d relo_relative_t;
    Quaterniond relo_relative_q;
    double relo_relative_yaw;

    cv::Mat latest_img;
    cv::Mat latest_depth;

    queue<pair<double,vector<Vector2d>>> ctd_pts_pub;
    queue<pair<double,cv::Mat>> rgb_img_pub;
    queue<pair<double,cv::Mat>> sdepth_pub;
    queue<pair<double,Matrix4d>> cpose_pub;
};
