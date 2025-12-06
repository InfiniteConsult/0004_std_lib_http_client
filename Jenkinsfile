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
                script {
                    def sonarProjectKey = sh(returnStdout: true, script: 'grep "^sonar.projectKey=" sonar-project.properties | cut -d= -f2').trim()

                    def sonarHostUrl = "http://sonarqube.cicd.local:9000"

                    withSonarQubeEnv('SonarQube') {
                        sh 'sonar-scanner'
                    }

                    // 3. Wait for Quality Gate
                    timeout(time: 5, unit: 'MINUTES') {
                        def qg = waitForQualityGate()
                        if (qg.status != 'OK') {
                            mattermostSend (
                                color: 'danger',
                                message: ":no_entry: **Quality Gate Failed**: ${qg.status}\n<${sonarHostUrl}/dashboard?id=${sonarProjectKey}|View Analysis>"
                            )
                            error "Pipeline aborted due to quality gate failure: ${qg.status}"
                        }
                    }
                }
            }
        }

        stage('Package') {
            steps {
                echo '--- Packaging Artifacts ---'
                sh 'mkdir -p dist'

                dir('build_release') {
                    sh 'cpack -G TGZ -C Release'
                    sh 'mv *.tar.gz ../dist/'
                }

                dir('src/rust') {
                    sh 'cargo package'
                    sh 'cp target/package/*.crate ../../dist/'
                }

                sh 'cp build_release/wheelhouse/*.whl dist/'
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

    // Global Post Actions
    post {
        failure {
            mattermostSend (
                color: 'danger',
                message: ":x: **Build Failed**\n**Job:** ${env.JOB_NAME} #${env.BUILD_NUMBER}\n(<${env.BUILD_URL}|Open Build>)"
            )
        }
        success {
            mattermostSend (
                color: 'good',
                message: ":white_check_mark: **Build Succeeded**\n**Job:** ${env.JOB_NAME} #${env.BUILD_NUMBER}\n(<${env.BUILD_URL}|Open Build>)"
            )
        }
    }
}