#include "glm/ext/matrix_transform.hpp"
#include "glm/trigonometric.hpp"
#include "transform.h"
#include <glm/ext/quaternion_trigonometric.hpp>
#include <stdexcept>
#define GLM_FORCE_RADIANS
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/string_cast.hpp>
#include <iostream>

#include "camera.hpp"

#define PI glm::pi<float>()
#define PI_2 glm::pi<float>()/2.0f
#define PRINT_FIXED_FLOAT(x, prec) std::fixed <<std::setprecision(prec)<<(x)

Camera::Camera(void){
}


Camera::~Camera(){
}


void Camera::Update() {
    // transform.Pitch(angular_velocity.x);
    // transform.Yaw(angular_velocity.y);
    // transform.Roll(angular_velocity.z);
    // transform.Translate(velocity);
    if(parent_transform) {
        transform.Update(parent_transform->GetWorldMatrix());
    } else {
        transform.Update();
    }

    float x = distance * glm::cos(orbit_pitch) * glm::cos(orbit_yaw);
    float y = distance * glm::sin(orbit_pitch);
    float z = distance * glm::cos(orbit_pitch) * glm::sin(orbit_yaw);
    transform.SetPosition({x,y,z});

    SetupViewMatrix();
}

void Camera::SetView(glm::vec3 position, glm::vec3 look_at, glm::vec3 up){

    // Store initial forward and side vectors
    // See slide in "Camera control" for details
    // probably will not be much used (directly setting view a rare occurrence in games)
    // transform.SetAxis(UP, up);
    // transform.SetAxis(FORWARD, glm::normalize(look_at - position));
    // transform.SetAxis(SIDE, glm::normalize(glm::cross(transform.GetAxis(FORWARD), up)));

    // Reset orientation and position of camera
    transform.SetPosition(position);
    transform.SetOrientation(glm::quat());
    original_pos = position;
    distance = glm::length(original_pos);
}


void Camera::SetPerspective(GLfloat fov, GLfloat near, GLfloat far, GLfloat w, GLfloat h){
    // Set projection based on field-of-view
    float top = tan((fov/2.0)*(glm::pi<float>()/180.0))*near;
    float right = top * w/h;
    perspective_matrix = glm::frustum(-right, right, -top, top, near, far);
}

void Camera::SetOrtho(GLfloat winwidth, GLfloat winheight){
    float w = (float)winwidth/2.0f;
    float h = (float)winheight/2.0f;
    ortho_matrix = glm::ortho<float>(-w, w, -h, h, -1.0f, 1.0f);
}


void Camera::SetProjectionUniforms(Shader& shd, Projection projtype){
    glm::mat4& projection = projtype == Projection::PERSPECTIVE ? perspective_matrix : ortho_matrix;
    shd.SetUniform4m(view_matrix_, "view_mat");
    shd.SetUniform4m(projection,  "projection_mat");
}


void Camera::SetupViewMatrix(void){

    if(parent_transform) {
        glm::mat4 p = parent_transform->GetLocalMatrix();
        glm::mat4 tr = transform.GetLocalMatrix();
        glm::vec3 eye = p * tr * glm::vec4(0.0, 0.0, 0.0, 1.0f);
        glm::vec3 look_at = p * glm::vec4(0.0, 0.0, 0.0, 1.0); // look slightly ahead of target
        // glm::vec3 look_at = p * transform.GetLocalMatrix() * glm::vec4(0.0, 0.0, -1.0, 1.0);
        glm::vec3 side = p * tr * glm::vec4(transform.GetAxis(Transform::SIDE), 0.0f);
        // glm::vec3 up = glm::cross(side, glm::vec3(p * glm::vec4(0.0, 0.0, -2.0, 0.0)));
        // glm::vec3 up = glm::cross(side, transform.GetAxis(FORWARD));
        glm::vec3 up = transform.GetAxis(Transform::UP);
        view_matrix_ = glm::lookAt(eye, look_at, up);
    } else {
        glm::vec3 eye = transform.GetPosition();
        glm::vec3 look_at = transform.GetLocalMatrix() * glm::vec4(0.0, 0.0, -1.0, 1.0);
        glm::vec3 up = transform.LocalAxis(Transform::UP);
        view_matrix_ = glm::lookAt(eye, look_at, up);
    }
}

bool Camera::IsAttached() {
    return parent_transform != nullptr;
}

void Camera::Attach(Transform *p) {
    if (IsAttached()) {
        Drop();
    }
    locked = true;
    // transform.SetPosition(original_pos); // reset local lock point
    parent_transform = p;
    Reset();
    SetupViewMatrix();
}

void Camera::Detach() {
    transform.SetOrientation(transform.GetWorldOrientation());
    parent_transform = nullptr;
    locked = false;
}

void Camera::Reset() {
    transform.SetOrientation(glm::quat(0, 0, 0, 0)); //reset local camera orientation
    transform.SetPosition(original_pos); // reset local lock point
    SetupViewMatrix();
}

void Camera::Drop() {
    transform.SetPosition(transform.GetWorldPosition()); 
    // inverse of the view matrix gives us the camera's transformation matrix
    transform.SetOrientation(glm::inverse(view_matrix_));
    parent_transform = nullptr;
    SetupViewMatrix();
    locked = false;
}

void Camera::MoveTo(const glm::vec3 newpos) {
    transform.SetPosition(newpos);
    SetupViewMatrix();
}

void Camera::OrbitPitch(float pitch) {
    float newpitch = orbit_pitch + pitch;
    if(newpitch + pitch > -PI/2.0f && newpitch + pitch < PI/2.0f) {
        orbit_pitch = newpitch;
    }
}

void Camera::OrbitYaw(float yaw) {
    float newyaw = orbit_yaw + yaw;
    orbit_yaw = newyaw;
}