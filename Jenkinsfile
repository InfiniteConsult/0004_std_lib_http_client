// Jenkinsfile

pipeline {
    // 1. Define our "Worker"
    // This tells Jenkins to spin up our custom-built agent
    // which already has all system dependencies (cmake, rust, python).
    agent {
        label 'general-purpose-agent'
    }

    stages {
        // 2. Setup & Build Stage
        // This runs the project's own setup.sh.
        // It will create the Python venv, install pip requirements,
        // and compile both the Debug and Release builds.
        stage('Setup & Build') {
            steps {
                echo '--- Running project setup.sh ---'
                sh 'chmod +x ./setup.sh'
                sh './setup.sh'
            }
        }

        // 3. Test & Coverage Stage
        // This runs the project's coverage script, which
        // depends on the 'build_debug' created in the prior stage.
        stage('Test & Coverage') {
            steps {
                echo '--- Running CTest, Cargo-Cov, and Pytest ---'
                sh 'chmod +x ./run-coverage.sh'
                sh './run-coverage.sh'
            }
        }
    }
}