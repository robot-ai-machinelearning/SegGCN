#include <cmath> // sqrtf
#include <cuda.h>
#include <cuda_runtime.h>
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/common_shape_fns.h"

using namespace tensorflow;

REGISTER_OP("BuildSphereNeighbor")
    .Attr("radius: float")          // range search radius
    .Attr("nn_sample: int")         // max number of neighbors sampled in the range
    .Input("database: float32")     // database points: batch * npoint * 3
    .Input("query: float32")        // query points: batch * mpoint * 3
    .Output("nn_index: int32")      // neighbor indices: batch * mpoint * nn_sample
    .Output("nn_count: int32")      // number of neighbors: batch * mpoint
    .Output("nn_dist: float32")     // distance to the neighbors: batch * mpoint * nn_sample
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
        ::tensorflow::shape_inference::ShapeHandle query; // batch * mpoint * 3
        c->WithRank(c->input(1), 3, &query);
        int nn_sample;
        TF_RETURN_IF_ERROR(c->GetAttr("nn_sample", &nn_sample));
        ::tensorflow::shape_inference::ShapeHandle nn_index = c->MakeShape({c->Dim(query, 0), c->Dim(query, 1), nn_sample});
        c->set_output(0, nn_index);
        ::tensorflow::shape_inference::ShapeHandle nn_count = c->MakeShape({c->Dim(query, 0), c->Dim(query, 1)});
        c->set_output(1, nn_count);
        ::tensorflow::shape_inference::ShapeHandle nn_dist = c->MakeShape({c->Dim(query, 0), c->Dim(query, 1), nn_sample});
        c->set_output(2, nn_dist);
        return Status::OK();
    });
REGISTER_OP("BuildNearestNeighbor")
    .Input("database: float32")     // database points: batch * npoint * 3
    .Input("query: float32")        // query points: batch * mpoint * 3
    .Output("nn_index: int32")      // neighbor indices: batch * mpoint * nn_out
    .Output("nn_dist: float32")     // distance to the neighbors: batch * mpoint * nn_out
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
        ::tensorflow::shape_inference::ShapeHandle query; // batch * mpoint * 3
        c->WithRank(c->input(1), 3, &query);
        int nn_out=3; // the number of nearest neighbors
        ::tensorflow::shape_inference::ShapeHandle nn_index = c->MakeShape({c->Dim(query, 0), c->Dim(query, 1), nn_out});
        c->set_output(0, nn_index);
        ::tensorflow::shape_inference::ShapeHandle nn_dist = c->MakeShape({c->Dim(query, 0), c->Dim(query, 1), nn_out});
        c->set_output(1, nn_dist);
        return Status::OK();
    });


void buildSphereNeighborLauncher(int B, int N, int M, int nnSample, float radius, const float* database,
                                 const float* query, int* nnIndex, int* nnCount, float* nnDist);
class BuildSphereNeighborGpuOp : public OpKernel {
    public:
        explicit BuildSphereNeighborGpuOp(OpKernelConstruction* context) : OpKernel(context)
        {
            OP_REQUIRES_OK(context, context->GetAttr("radius", &radius_));
            OP_REQUIRES(context, radius_ > 0, errors::InvalidArgument("Range search requires radius>0, got ", radius_));

            OP_REQUIRES_OK(context, context->GetAttr("nn_sample", &nn_sample_));
            OP_REQUIRES(context, nn_sample_ > 0, errors::InvalidArgument("BuildSphereNeighbor requires nn_sample>0, got ", nn_sample_));
        }

        void Compute(OpKernelContext* context) override {
            // Grab the input tensors
            const Tensor& database_tensor = context->input(0);
            const Tensor& query_tensor = context->input(1);

            // get the dims required by computations
            int B = database_tensor.shape().dim_size(0); // batch size
            int N = database_tensor.shape().dim_size(1); // number of database points
            int M = query_tensor.shape().dim_size(1);    // number of query points

            OP_REQUIRES(context, database_tensor.dims()==3 && database_tensor.shape().dim_size(2)==3, errors::InvalidArgument("Shape of database points requires to be (batch, npoint, 3)"));
            OP_REQUIRES(context, query_tensor.dims()==3 && query_tensor.shape().dim_size(2)==3, errors::InvalidArgument("Shape of query points requires to be (batch, mpoint, 3)"));

            // flatten the input tensors
            auto database_flat = database_tensor.flat<float>();
            auto query_flat = query_tensor.flat<float>();

            // Create an output tensor
            Tensor* nn_index_tensor = NULL;
            Tensor* nn_count_tensor = NULL;
            Tensor* nn_dist_tensor = NULL;
            OP_REQUIRES_OK(context, context->allocate_output(0, TensorShape{B,M,nn_sample_}, &nn_index_tensor));
            OP_REQUIRES_OK(context, context->allocate_output(1, TensorShape{B,M}, &nn_count_tensor));
            OP_REQUIRES_OK(context, context->allocate_output(2, TensorShape{B,M,nn_sample_}, &nn_dist_tensor));
            auto nn_index_flat = nn_index_tensor->flat<int32>();
            auto nn_count_flat = nn_count_tensor->flat<int32>();
            auto nn_dist_flat = nn_dist_tensor->flat<float>();

            const float* database = &(database_flat(0));
            const float* query = &(query_flat(0));
            int* nnIndex = &(nn_index_flat(0));
            int* nnCount = &(nn_count_flat(0));
            float* nnDist = &(nn_dist_flat(0));

            cudaMemset(nnIndex, 0, sizeof(int)*B*M*nn_sample_);
            cudaMemset(nnCount, 0, sizeof(int)*B*M);
            cudaMemset(nnDist, 0, sizeof(float)*B*M*nn_sample_);

            buildSphereNeighborLauncher(B, N, M, nn_sample_, radius_,
                           database, query, nnIndex, nnCount, nnDist);
        }
    private:
        float radius_;
        int nn_sample_;
};
REGISTER_KERNEL_BUILDER(Name("BuildSphereNeighbor").Device(DEVICE_GPU), BuildSphereNeighborGpuOp);


void buildNearestNeighborLauncher(int B, int N, int M, const float* database,
                                  const float* query, int* nnIndex, float* nnDist);
class BuildNearestNeighborGpuOp : public OpKernel {
    public:
        explicit BuildNearestNeighborGpuOp(OpKernelConstruction* context) : OpKernel(context)
        {
        }

        void Compute(OpKernelContext* context) override {
            // Grab the input tensors
            const Tensor& database_tensor = context->input(0);
            const Tensor& query_tensor = context->input(1);

            // get the dims required by computations
            int B = database_tensor.shape().dim_size(0); // batch size
            int N = database_tensor.shape().dim_size(1); // number of database points
            int M = query_tensor.shape().dim_size(1);    // number of query points

            OP_REQUIRES(context, database_tensor.dims()==3 && database_tensor.shape().dim_size(2)==3, errors::InvalidArgument("Shape of database points requires to be (batch, npoint, 3)"));
            OP_REQUIRES(context, query_tensor.dims()==3 && query_tensor.shape().dim_size(2)==3, errors::InvalidArgument("Shape of query points requires to be (batch, mpoint, 3)"));

            // flatten the input tensors
            auto database_flat = database_tensor.flat<float>();
            auto query_flat = query_tensor.flat<float>();

            // Create an output tensor
            Tensor* nn_index_tensor = NULL;
            Tensor* nn_dist_tensor = NULL;
            OP_REQUIRES_OK(context, context->allocate_output(0, TensorShape{B,M,nn_out_}, &nn_index_tensor));
            OP_REQUIRES_OK(context, context->allocate_output(1, TensorShape{B,M,nn_out_}, &nn_dist_tensor));
            auto nn_index_flat = nn_index_tensor->flat<int32>();
            auto nn_dist_flat = nn_dist_tensor->flat<float>();

            const float* database = &(database_flat(0));
            const float* query = &(query_flat(0));
            int* nnIndex = &(nn_index_flat(0));
            float* nnDist = &(nn_dist_flat(0));

            cudaMemset(nnIndex, 0, sizeof(int)*B*M*nn_out_);
            cudaMemset(nnDist, 0, sizeof(float)*B*M*nn_out_);

            buildNearestNeighborLauncher(B, N, M, database, query, nnIndex, nnDist);
        }
    private:
        const int nn_out_ = 3; // find 3 nearest neighbors
};
REGISTER_KERNEL_BUILDER(Name("BuildNearestNeighbor").Device(DEVICE_GPU), BuildNearestNeighborGpuOp);








