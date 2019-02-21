#include "pch.h"
#include "ShapeUnderClothOptimizer.h"


ShapeUnderClothOptimizer::ShapeUnderClothOptimizer(SMPLWrapper* smpl, GeneralMesh* input, const char* path_to_prior)
{
    this->smpl_ = smpl;
    this->input_ = input;

    assert(input->getVertices().cols() == SMPLWrapper::SPACE_DIM && "World dimentions should be equal for SMPL and input mesh");

    // read prior info
    std::string path(path_to_prior);
    path += '/';
    this->readMeanPose_(path);
    this->readStiffness_(path);
}


ShapeUnderClothOptimizer::~ShapeUnderClothOptimizer()
{
    this->erase_params_();
}


void ShapeUnderClothOptimizer::setNewSMPLModel(SMPLWrapper* smpl)
{
    this->smpl_ = smpl;
}


void ShapeUnderClothOptimizer::setNewInput(GeneralMesh * input)
{
    this->input_ = input;
}


void ShapeUnderClothOptimizer::setNewPriorPath(const char * prior_path)
{
    std::string path(prior_path);
    this->readMeanPose_(path);
    this->readStiffness_(path);
}


double * ShapeUnderClothOptimizer::getEstimatesTranslationParams()
{
    if (this->translation_ != nullptr)
    {
        double * last_translation = new double[SMPLWrapper::SPACE_DIM];
        for (int i = 0; i < SMPLWrapper::SPACE_DIM; i++)
            last_translation[i] = this->translation_[i];
        return last_translation;
    }
    else
        return nullptr;
}


double * ShapeUnderClothOptimizer::getEstimatesPoseParams()
{
    if (this->pose_ != nullptr)
    {
        double * last_pose = new double[SMPLWrapper::POSE_SIZE];
        for (int i = 0; i < SMPLWrapper::POSE_SIZE; i++)
            last_pose[i] = this->pose_[i];
        return last_pose;
    }
    else
        return nullptr;
}


double * ShapeUnderClothOptimizer::getEstimatesShapeParams()
{
    if (this->shape_ != nullptr)
    {
        double * last_shape = new double[SMPLWrapper::SHAPE_SIZE];
        for (int i = 0; i < SMPLWrapper::SHAPE_SIZE; i++)
            last_shape[i] = this->shape_[i];
        return last_shape;
    }
    else 
        return nullptr;
}


void ShapeUnderClothOptimizer::findOptimalParameters(std::vector<Eigen::MatrixXd>* iteration_results)
{
    google::InitGoogleLogging("ShapeUnderClothing");

    // Init parameters
    this->erase_params_();
    this->translation_ = new double[SMPLWrapper::SPACE_DIM];
    this->pose_ = new double[SMPLWrapper::POSE_SIZE];
    this->shape_ = new double[SMPLWrapper::SHAPE_SIZE];

    E::VectorXd translation_guess = this->input_->getMeanPoint() - this->smpl_->getTemplateMeanPoint();
    assert(translation_guess.size() == SMPLWrapper::SPACE_DIM 
        && "Calculated translation guess should have size equal to the SMPL world dimentionality");
    for (int i = 0; i < SMPLWrapper::SPACE_DIM; ++i)
        this->translation_[i] = translation_guess(i);
    this->zeros_(this->pose_, SMPLWrapper::POSE_SIZE);
    this->zeros_(this->shape_, SMPLWrapper::SHAPE_SIZE);

    // Setup solvers options
    Solver::Options options;
    options.linear_solver_type = ceres::DENSE_QR;   // analytic jacobian is dense
    options.minimizer_progress_to_stdout = true;
    options.max_num_iterations = 500;   // usually converges faster

    // to debug jacobian
    //options.check_gradients = true;

    SMPLVertsLoggingCallBack* callback = nullptr;
    if (iteration_results != nullptr)
    {
        callback = new SMPLVertsLoggingCallBack(this->smpl_, this->pose_, this->shape_, this->translation_, iteration_results);
        options.callbacks.push_back(callback);
        options.update_state_every_iteration = true;
    }

    // parameters estimation
    this->directionalPoseEstimation_(options);

    //this->generalPoseEstimation_(options);

    // cleanup
    if (callback != nullptr)
    {
        delete callback;
    }
}


void ShapeUnderClothOptimizer::directionalPoseEstimation_(Solver::Options & options)
{
    Problem problem;

    // Main cost
    CostFunction* dir_based_cost_function = new DirBasedDistanceForPose(this->smpl_, this->input_);
    problem.AddResidualBlock(dir_based_cost_function, nullptr, this->pose_);

    // Regularizer
    CostFunction* prior = new NormalPrior(this->stiffness_, this->mean_pose_);
    LossFunction* scale_prior = new ScaledLoss(NULL, 0.0001, ceres::TAKE_OWNERSHIP);
    problem.AddResidualBlock(prior, scale_prior, this->pose_);

    // Run the solver!
    Solver::Summary summary;
    Solve(options, &problem, &summary);

    // Print summary
    std::cout << "Summary:" << std::endl;
    std::cout << summary.FullReport() << std::endl;

#ifdef DEBUG
    // Print last Gradient (?) and Jacobian
    std::vector<double> gradient;
    ceres::CRSMatrix jac;
    problem.Evaluate(Problem::EvaluateOptions(), NULL, NULL, &gradient, &jac);
    std::cout << "Gradient" << std::endl;
    for (auto i = gradient.begin(); i != gradient.end(); ++i)
    {
        std::cout << *i << " ";
    }
    std::cout << std::endl;

    std::cout << "Jacobian (sparse)" << std::endl;

    for (int i = 0; i < jac.num_rows; ++i)
    {
        for (int j = jac.rows[i]; j < jac.rows[i + 1]; ++j)   // only pose jacobian
        {
            std::cout << jac.values[j] << " ";
        }
        std::cout << std::endl;
    }

#endif // DEBUG

}


void ShapeUnderClothOptimizer::generalPoseEstimation_(Solver::Options& options)
{
    Problem problem;

    // Main cost
    CostFunction* cost_function = new AbsoluteVertsToMeshDistance(this->smpl_, this->input_);
    problem.AddResidualBlock(cost_function, nullptr, this->pose_, this->translation_);     // this->pose_, , this->shape_ 

    // Regularizer
    CostFunction* prior = new NormalPrior(this->stiffness_, this->mean_pose_);
    LossFunction* scale_prior = new ScaledLoss(NULL, 0.001, ceres::TAKE_OWNERSHIP);
    problem.AddResidualBlock(prior, scale_prior, this->pose_);

    // Run the solver!
    Solver::Summary summary;
    Solve(options, &problem, &summary);

    // Print summary
    std::cout << "Summary:" << std::endl;
    std::cout << summary.FullReport() << std::endl;

#ifdef DEBUG
    // Print last Gradient (?) and Jacobian
    std::vector<double> gradient;
    ceres::CRSMatrix jac;
    problem.Evaluate(Problem::EvaluateOptions(), NULL, NULL, &gradient, &jac);
    std::cout << "Gradient" << std::endl;
    for (auto i = gradient.begin(); i != gradient.end(); ++i)
    {
        std::cout << *i << " ";
    }
    std::cout << std::endl;

    std::cout << "Jacobian (sparse)" << std::endl;

    for (int i = 0; i < jac.num_rows; ++i)
    {
        for (int j = jac.rows[i]; j < jac.rows[i + 1]; ++j)   // only pose jacobian
        {
            std::cout << jac.values[j] << " ";
        }
        std::cout << std::endl;
    }

#endif // DEBUG
}

void ShapeUnderClothOptimizer::erase_params_()
{
    if (this->translation_ != nullptr)
    {
        delete[] this->translation_;
        this->translation_ = nullptr;
    }

    if (this->pose_ != nullptr)
    {
        delete[] this->pose_;
        this->pose_ = nullptr;
    }

    if (this->shape_ != nullptr)
    {
        delete[] this->shape_;
        this->shape_ = nullptr;
    }
}


void ShapeUnderClothOptimizer::readMeanPose_(const std::string path)
{
    std::string mean_filename = path + "mean_pose.txt";

    std::fstream inFile;
    inFile.open(mean_filename, std::ios_base::in);
    int size;
    inFile >> size;
    // Sanity check
    if (size != SMPLWrapper::POSE_SIZE - SMPLWrapper::SPACE_DIM)
        throw std::exception("Striffness matrix size doesn't match the number of non-root pose parameters");
    
    this->mean_pose_.resize(SMPLWrapper::POSE_SIZE);
    // For convinient use of the mean pose with full pose vectors, root rotation is set to zero
    for (int i = 0; i < SMPLWrapper::SPACE_DIM; i++)
        this->mean_pose_(i) = 0.;
    // Now read from file
    for (int i = SMPLWrapper::SPACE_DIM; i < SMPLWrapper::POSE_SIZE; i++)
        inFile >> this->mean_pose_(i);

    inFile.close();

#ifdef DEBUG
    std::cout << "Read mean pose" << std::endl;
    //for (int i = 0; i < SMPLWrapper::POSE_SIZE; i++)
    //{
    //    std::cout << this->mean_pose_(i) << " ";
    //}
    //std::cout << std::endl;
#endif // DEBUG
}


void ShapeUnderClothOptimizer::readStiffness_(const std::string path)
{
    std::string stiffness_filename = path + "stiffness.txt";

    std::fstream inFile;
    inFile.open(stiffness_filename, std::ios_base::in);
    constexpr int NON_ROOT_POSE_SIZE = SMPLWrapper::POSE_SIZE - SMPLWrapper::SPACE_DIM;
    int rows, cols;
    inFile >> rows;
    inFile >> cols;
    // Sanity check
    if (rows != cols)
        throw std::exception("Striffness matrix is not a square matrix");
    if (rows != SMPLWrapper::POSE_SIZE - SMPLWrapper::SPACE_DIM)
        throw std::exception("Striffness matrix size doesn't match the number of non-root pose parameters");

    // To make matrix applicable to full pose vector
    this->stiffness_.resize(SMPLWrapper::POSE_SIZE, SMPLWrapper::POSE_SIZE);
    for (int i = 0; i < SMPLWrapper::SPACE_DIM; i++)
        for (int j = 0; j < SMPLWrapper::POSE_SIZE; j++)
            this->stiffness_(i, j) = this->stiffness_(j, i) = 0.;

    // Now read from file
    for (int i = SMPLWrapper::SPACE_DIM; i < SMPLWrapper::POSE_SIZE; i++)
        for (int j = SMPLWrapper::SPACE_DIM; j < SMPLWrapper::POSE_SIZE; j++)
            inFile >> this->stiffness_(i, j);

    inFile.close();

#ifdef DEBUG
    std::cout << "Read stiffness matrix" << std::endl;
    //for (int i = 0; i < SMPLWrapper::POSE_SIZE; i++)
    //{
    //    for (int j = 0; j < SMPLWrapper::POSE_SIZE; j++)
    //    {
    //        std::cout << this->stiffness_(i, j) << " ";
    //    }
    //    std::cout << std::endl;
    //}
    
#endif // DEBUG

}


void ShapeUnderClothOptimizer::zeros_(double * arr, std::size_t size)
{
    for (int i = 0; i < size; i++)
    {
        arr[i] = 0.;
    }
}


void ShapeUnderClothOptimizer::printArray_(double * arr, std::size_t size)
{
    for (int i = 0; i < size; i++)
    {
        std::cout << arr[i] << "; ";
    }
    std::cout << std::endl;
}


ceres::CallbackReturnType ShapeUnderClothOptimizer::SMPLVertsLoggingCallBack::operator()(const ceres::IterationSummary & summary)
{
    Eigen::MatrixXd verts = this->smpl_->calcModel(this->pose_, this->shape_);
    for (int i = 0; i < SMPLWrapper::VERTICES_NUM; ++i)
        for (int j = 0; j < SMPLWrapper::SPACE_DIM; ++j)
            verts(i, j) += this->translation_[j];
    this->smpl_verts_results_->push_back(verts);

    return ceres::SOLVER_CONTINUE;
}
