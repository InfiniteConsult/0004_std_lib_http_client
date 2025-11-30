pipeline {
    agent {
        label 'general-purpose-agent'
    }

    stages {
        stage('Setup & Build') {
            steps {
                echo '--- Building Project ---'
                sh 'chmod +x ./setup.sh'
                sh './setup.sh'
            }
        }

        stage('Test & Coverage') {
            steps {
                echo '--- Running Tests ---'
                sh 'chmod +x ./run-coverage-cicd.sh'
                sh './run-coverage-cicd.sh'
            }
        }

        stage('Code Analysis') {
            steps {
                withSonarQubeEnv('SonarQube') {
                    // 1. Run the scanner (Uploads the report)
                    sh 'sonar-scanner'
                }

                // 2. Wait for the Webhook callback
                // This pauses the pipeline until SonarQube finishes processing
                timeout(time: 5, unit: 'MINUTES') {
                    waitForQualityGate abortPipeline: true
                }
            }
        }

        stage('Package') {
            steps {
                echo '--- Packaging Artifacts ---'
                sh 'mkdir -p dist'

                // 1. Package C/C++ SDK (CPack)
                // We run inside build_release to access the CMake cache
                dir('build_release') {
                    sh 'cpack -G TGZ -C Release'
                    // Move the resulting tarball to dist/
                    sh 'mv *.tar.gz ../dist/'
                }

                // 2. Package Rust Crate
                // We run inside src/rust. No --allow-dirty needed on a fresh CI node.
                dir('src/rust') {
                    sh 'cargo package'
                    // Copy the crate from target/package to dist/
                    sh 'cp target/package/*.crate ../../dist/'
                }

                // 3. Collect Python Wheel
                // The wheel was built by setup.sh in build_release/wheelhouse
                sh 'cp build_release/wheelhouse/*.whl dist/'

                // Verify
                sh 'ls -l dist/'
            }
        }

        stage('Publish') {
            steps {
                echo '--- Publishing to Artifactory ---'

                rtUpload (
                    serverId: 'artifactory',
                    spec: """{
                          "files": [
                            {
                              "pattern": "dist/*",
                              "target": "generic-local/http-client/${BUILD_NUMBER}/",
                              "flat": "true"
                            }
                          ]
                    }""",
                    failNoOp: true,
                    buildName: "${JOB_NAME}",
                    buildNumber: "${BUILD_NUMBER}"
                )

                rtPublishBuildInfo (
                    serverId: 'artifactory',
                    buildName: "${JOB_NAME}",
                    buildNumber: "${BUILD_NUMBER}"
                )
            }
        }
    }
}