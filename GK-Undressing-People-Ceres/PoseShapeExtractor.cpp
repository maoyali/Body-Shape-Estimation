#include "PoseShapeExtractor.h"

// init statics
int PoseShapeExtractor::iteration_viewer_counter_;
std::shared_ptr <VertsVector> PoseShapeExtractor::iteration_outputs_to_viz_;
std::shared_ptr <SMPLWrapper> PoseShapeExtractor::smpl_to_viz_;
std::shared_ptr <GeneralMesh> PoseShapeExtractor::input_to_viz_;

PoseShapeExtractor::PoseShapeExtractor(const std::string& smpl_model_path,
    const std::string& open_pose_path,
    const std::string& pose_prior_path,
    const std::string& logging_path)
    : smpl_model_path_(smpl_model_path), openpose_model_path_(open_pose_path), 
    pose_prior_path_(pose_prior_path), logging_base_path_(logging_path)
{
    smpl_ = nullptr;
    input_ = nullptr;
    logger_ = nullptr;
    openpose_ = nullptr;

    cameras_distance_ = 4.5f;
    num_cameras_ = 7;
    cameras_elevation_ = 0.0;

    optimizer_ = std::make_shared<ShapeUnderClothOptimizer>(nullptr, nullptr, pose_prior_path_);

    // as glog is used by class members
    google::InitGoogleLogging("PoseShapeExtractor");
}

PoseShapeExtractor::~PoseShapeExtractor()
{}

void PoseShapeExtractor::setupNewExperiment(std::shared_ptr<GeneralMesh> input, const std::string experiment_name)
{
    input_ = std::move(input);
    logger_ = std::make_shared<CustomLogger>(logging_base_path_, experiment_name + "_" + input_->getName());

    // for convenience
    input_->saveNormalizedMesh(logger_->getLogFolderPath());

    // update tools
    openpose_ = nullptr;
    char input_gender = convertInputGenderToChar_(*input_.get());
    smpl_ = std::make_shared<SMPLWrapper>(input_gender, smpl_model_path_);
}

void PoseShapeExtractor::setupNewCameraExperiment(std::shared_ptr<GeneralMesh> input, 
    double distance, int n_cameras, double elevation, const std::string experiment_name)
{
    cameras_distance_ = distance;
    num_cameras_ = n_cameras;
    cameras_elevation_ = elevation;
    setupNewExperiment(std::move(input),
        experiment_name + "_n_" + std::to_string(n_cameras) 
        + "_dist_" + std::to_string((int)(distance*10)) 
        + "_Y_" + std::to_string((int)(elevation * 10)));
}

std::shared_ptr<SMPLWrapper> PoseShapeExtractor::runExtraction()
{
    if (input_ == nullptr)
        throw std::exception("PoseShapeExtractor: ERROR: You asked to run extraction before setting up the experiment.");
    // 1.
    takePhotos_();
    
    // 2.
    estimateInitialPoseWithOP_();

    // 3.
    runPoseShapeOptimization_();

    // 4.
    logger_->saveFinalModel(*smpl_);
    if (save_iteration_results_)
        logger_->saveIterationsSMPLObjects(*smpl_, iteration_outputs_);

    return smpl_;
}

void PoseShapeExtractor::viewCameraSetupForPhotos()
{
    if (input_ == nullptr)
    {
        throw std::exception("PoseShapeExtractor: need some input specified to show the cameras scene. Sorry 0:)");
    }

    Photographer photographer(input_.get());

    photoSetUp_(photographer);
    
    photographer.viewScene();
}

void PoseShapeExtractor::viewFinalResult(bool withOpenPoseKeypoints)
{
    igl::opengl::glfw::Viewer viewer;
    igl::opengl::glfw::imgui::ImGuiMenu menu;
    viewer.plugins.push_back(&menu);

    viewer.data().set_mesh(smpl_->calcModel(), smpl_->getFaces());

    if (withOpenPoseKeypoints)
    {
        if (openpose_ == nullptr)
        {
            throw std::exception("PoseShapeExtractor: openpose keypoints are "
                " unavalible for Visualization. Run extraction first.");
        }
        Eigen::MatrixXd op_keypoints = openpose_->getKeypoints();
        op_keypoints = op_keypoints.block(0, 0, op_keypoints.rows(), 3);

        viewer.data().set_points(op_keypoints, Eigen::RowVector3d(1., 1., 0.));
    }
    
    viewer.launch();
}

void PoseShapeExtractor::viewIteratoinProcess()
{
    if (iteration_outputs_.size() > 0)
    {
        // fill satic vars to be used in visualization
        iteration_outputs_to_viz_ = std::shared_ptr<VertsVector> (&iteration_outputs_);
        smpl_to_viz_ = smpl_;
        input_to_viz_ = input_;

        igl::opengl::glfw::Viewer viewer;
        igl::opengl::glfw::imgui::ImGuiMenu menu;
        viewer.plugins.push_back(&menu);

        iteration_viewer_counter_ = 0;
        viewer.callback_key_down = &visualizeIterationKeyDown_;
        viewer.callback_pre_draw = &visualizeIterationPreDraw_;
        viewer.core().is_animating = false;
        viewer.core().animation_max_fps = 24.;
        std::cout << "Press [space] to toggle animation or [Shift+F] to see the final result." << std::endl;
        viewer.launch();
    }
    else
    {
        std::cout << "PoseShapeExtractor: "
            << "I skipped visualization since iteration results were not collected." << std::endl;
    }
}

/// Private: ///

void PoseShapeExtractor::photoSetUp_(Photographer& photographer)
{
    const double pi = 3.141592653589793238463;

    // put cameras all around the target
    double circle_segment = 2. * pi / num_cameras_;
    double shift = 0.2;     // for fun. On a particular input, 5 cam + zero shift openpose faled miserably

    for (int i = 0; i < num_cameras_; i++)
    {
        photographer.addCameraToPosition(
            cos(shift + circle_segment * i), cameras_elevation_, sin(shift + circle_segment * i),
            cameras_distance_);
    }
}

void PoseShapeExtractor::takePhotos_()
{
    std::cout << "PoseShapeExtractor: I'm taking photos of the input!" << std::endl;

    Photographer photographer(input_.get());

    photoSetUp_(photographer);

    photographer.renderToImages(logger_->getPhotosFolderPath());
    photographer.saveImageCamerasParamsCV(logger_->getPhotosFolderPath());
}

void PoseShapeExtractor::estimateInitialPoseWithOP_()
{
    std::cout << "PoseShapeExtractor: I'm estimating the pose with OpenPose!" << std::endl;
    if (openpose_ == nullptr)
    {
        openpose_ = std::make_shared<OpenPoseWrapper>(logger_->getPhotosFolderPath(),
            logger_->getPhotosFolderPath(), num_cameras_,
            logger_->getOpenPoseGuessesPath(),
            openpose_model_path_);
    }
    
    openpose_->runPoseEstimation();

    // Map OpenPose pose to SMPL /////
    logger_->startRedirectCoutToFile("mapping_process_info.txt");
    openpose_->mapToSmpl(*smpl_);
    logger_->endRedirectCoutToFile();
}

void PoseShapeExtractor::runPoseShapeOptimization_()
{
    // update the data in the oprimizer in case it changed
    optimizer_->setNewInput(input_.get());
    optimizer_->setNewSMPLModel(smpl_.get());

    std::cout << "Starting optimization...\n";

    double expetiment_param = 0.;
    
    logger_->startRedirectCoutToFile("optimization.txt");
    std::cout << "Input file: " << input_->getName() << std::endl;

    iteration_outputs_.clear();
    if (save_iteration_results_)
    {
        optimizer_->findOptimalParameters(&iteration_outputs_, expetiment_param);
    }
    else
    {
        optimizer_->findOptimalParameters(nullptr, expetiment_param);
    }
    
    logger_->endRedirectCoutToFile();
    std::cout << "Optimization finished!\n";
}

char PoseShapeExtractor::convertInputGenderToChar_(const GeneralMesh& input)
{
    char gender;
    switch (input.getGender())
    {
    case GeneralMesh::FEMALE:
        gender = 'f';
        break;
    case GeneralMesh::MALE:
        gender = 'm';
        break;
    default:
        gender = 'u';
    }

    return gender;
}

bool PoseShapeExtractor::visualizeIterationPreDraw_(igl::opengl::glfw::Viewer & viewer)
{
    if (viewer.core().is_animating && iteration_viewer_counter_ < iteration_outputs_to_viz_->size())
    {
        viewer.data().clear();
        Eigen::MatrixXi faces = smpl_to_viz_->getFaces();

        viewer.data().set_mesh((*iteration_outputs_to_viz_)[iteration_viewer_counter_], faces);
        viewer.core().align_camera_center((*iteration_outputs_to_viz_)[iteration_viewer_counter_], faces);

        iteration_viewer_counter_++;
    }
    else if (viewer.core().is_animating && iteration_viewer_counter_ >= iteration_outputs_to_viz_->size())
    {
        viewer.core().is_animating = false;
        iteration_viewer_counter_ = 0;
        std::cout << "You can start the animation again by pressing [space]" << std::endl;
    }
    return false;
}

bool PoseShapeExtractor::visualizeIterationKeyDown_(igl::opengl::glfw::Viewer & viewer, unsigned char key, int modifier)
{
    if (key == ' ')
    {
        viewer.core().is_animating = !viewer.core().is_animating;
    }
    else if (key == 'F')
    {
        std::cout << "[Shift+F] pressed: Showing the final result."
            << "Press [space] to go back to animation mode." << std::endl;

        viewer.core().is_animating = false;

        // visualizing the final result only
        viewer.data().clear();
        Eigen::MatrixXi faces = smpl_to_viz_->getFaces();
        Eigen::MatrixXd verts = (*iteration_outputs_to_viz_)[iteration_outputs_to_viz_->size() - 1];
        viewer.data().set_mesh(verts, faces);


        Eigen::VectorXd sqrD;
        Eigen::MatrixXd closest_points;
        Eigen::VectorXi closest_face_ids;
        igl::point_mesh_squared_distance(verts,
            input_to_viz_->getVertices(), input_to_viz_->getFaces(), sqrD,
            closest_face_ids, closest_points);

        viewer.data().add_edges(verts, closest_points, Eigen::RowVector3d(1., 0., 0.));

        // visualize joint locations
        Eigen::MatrixXd finJointLocations = smpl_to_viz_->calcJointLocations();
        for (int i = 0; i < finJointLocations.rows(); ++i)
        {
            for (int j = 0; j < SMPLWrapper::SPACE_DIM; ++j)
            {
                finJointLocations(i, j) += smpl_to_viz_->getStatePointers().translation[j];
            }
        }
        viewer.data().add_points(finJointLocations, Eigen::RowVector3d(1., 1., 0.));
    }
    return false;
}
