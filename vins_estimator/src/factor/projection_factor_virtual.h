#include <ros/assert.h>
#include <ceres/ceres.h>
#include <Eigen/Dense>
#include "../utility/utility.h"
#include "../parameters.h"
#include <malloc.h>
#include <ceres/rotation.h>

using namespace Eigen;

struct ProjectionFactorVirtual
{
    ProjectionFactorVirtual(){}

    template <typename T>
    bool operator()(const T* const inv_depth_,
                    const T* const depth_,
                    T* residuals) const
    {
        const T inv_depth = inv_depth_[0];
        const T depth = depth_[0];
//        const T k = 1;



//        residuals[0] = pow(depth - static_cast<T>(1)/inv_depth,2);
        residuals[0] = pow(inv_depth - inv_depth,2);

        return true;
    }

private:

};


