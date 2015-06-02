// Copyright (c) 2015, Galaxy Authors. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: wangtaize@baidu.com

#include "agent/task_runner.h"

#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sstream>
#include <sys/types.h>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <pwd.h>
#include "common/logging.h"
#include "common/util.h"
#include "downloader_manager.h"
#include "agent/resource_collector_engine.h"
#include "agent/utils.h"
#include <gflags/gflags.h>
 
DECLARE_int32(task_retry_times);
DECLARE_string(task_acct);

namespace galaxy {

static const std::string RUNNER_META_PREFIX = "task_runner_";

int AbstractTaskRunner::IsProcessRunning(pid_t process){
    if ((int64_t)process == -1) {
        LOG(WARNING, "process %d is not running",(int64_t)process);
        return -1;
    }
    // check process exist
    int ret = ::kill(process, 0);
    if(ret == 0 ){
        //check process status
        pid_t pid = waitpid(process, &ret, WNOHANG);
        if(pid == 0 ){
            LOG(INFO,"process %d is running", process);
            return 0;
        }else if(pid == -1){
            LOG(WARNING,"fail to check process %d state [%d,%s]",
                    process, errno, strerror(errno));
            return -1;
        }
        else{
            if(WIFEXITED(ret)){
                int exit_code = WEXITSTATUS(ret);
                if(exit_code == 0 ){
                    //normal exit
                    LOG(INFO,"process %d exits successfully", process);
                    return 1;
                }
                LOG(FATAL,"process %d exits with err code %d", process, exit_code);
            }
            return -1;
        }

    }
    LOG(INFO, "check pid %d error[%d:%s] ",
            process, ret, strerror(errno));
    return ret;
}

int AbstractTaskRunner::IsRunning() {
    
    int ret = IsProcessRunning(m_child_pid);
    if (ret != 0) {
        LOG(WARNING, "task with id %ld not running with pid %ld",
                m_task_info.task_id(),
                (int64_t)m_child_pid);
        return ret;
    }
    return ret;
}

void AbstractTaskRunner::SetStatus(int status) {
    LOG(DEBUG, "task with id %ld change state from %d to %d",
            m_task_info.task_id(),
            m_task_state,
            status);
    m_task_state = status;
}

void AbstractTaskRunner::AsyncDownload(boost::function<void()> callback) {
    std::string uri = m_task_info.task_raw();
    std::string path = m_workspace->GetPath();
    path.append("/");
    path.append("tmp.tar.gz");
    SetStatus(DEPLOYING);
    // set deploying state
    DownloaderManager* downloader_handler = DownloaderManager::GetInstance();
    downloader_id_ = downloader_handler->DownloadInThread(
                        uri,
                        path,
                        boost::bind(&AbstractTaskRunner::StartAfterDownload, this, callback, _1));
    return;
}

void AbstractTaskRunner::StartAfterDownload(boost::function<void()> callback, int ret) {
    if (ret == 0) {
        std::string tar_cmd = "cd "
            + m_workspace->GetPath()
            + " && tar -xzf tmp.tar.gz";
        int status = system(tar_cmd.c_str());
        if (status != 0) {
            LOG(WARNING, "task with id %ld extract failed",
                    m_task_info.task_id());
        }
        else {
            callback();
            return;
        }
    }
    LOG(WARNING, "task with id %ld deploy failed",
            m_task_info.task_id());
    SetStatus(ERROR);
    return;
}

int AbstractTaskRunner::Stop(){
    if (m_task_state == DEPLOYING) {
        // do download stop
        DownloaderManager* downloader_handler = DownloaderManager::GetInstance();
        downloader_handler->KillDownload(downloader_id_);
        LOG(DEBUG, "task id %ld stop failed with deploying", m_task_info.task_id());
        return -1;
    }

    // when stop success, but clear cgroup failed
    if (m_group_pid == -1) {
        return 0; 
    }

    // TODO pid reuse will cause some trouble
    LOG(INFO,"start to kill process group %d",m_group_pid);
    do {
        int ret = killpg(m_group_pid, SIGKILL);
        if (ret != 0) {
            LOG(WARNING,"fail to kill process group %d err[%d: %s]",
                    m_group_pid, errno, strerror(errno));
            if (errno == ESRCH) {
                break; 
            }
        }
        pid_t killed_pid = wait(&ret);
        if (killed_pid == -1) {
            LOG(FATAL,"fail to kill process group %d err[%d: %s]",
                    m_group_pid, errno, strerror(errno));
            SetStatus(ERROR);
            return -1;
        } 
    } while (0);
    StopPost();
    LOG(INFO,"kill child process %d successfully", m_group_pid);
    m_child_pid = -1;
    m_group_pid = -1;
    //kill monitor
    if (IsProcessRunning(m_monitor_pid) != 0) {
        return 0;
    }
    LOG(INFO,"start to kill monitor group %d",m_monitor_gid);
    do {
        int ret = killpg(m_monitor_gid, 9);
        if(ret != 0){
            LOG(WARNING,"fail to kill monitor group %d err[%d: %s]",
                    m_monitor_gid, errno, strerror(errno));
            if (errno == ESRCH) {
                break;
            }
        }
        pid_t killed_pid = wait(&ret);
        if (killed_pid == -1) {
            LOG(FATAL,"fail to kill monitor group %d",m_monitor_gid);
            return -1;
        }
    }while(0);        
    LOG(INFO,"kill monitor %d successfully", m_monitor_pid);
    m_monitor_pid = -1;
    m_monitor_gid = -1;

    return 0;
}

void AbstractTaskRunner::PrepareStart(std::vector<int>& fd_vector,int* stdout_fd,int* stderr_fd){
    pid_t current_pid = getpid();
    common::util::GetProcessFdList(current_pid, fd_vector);
    std::string task_stdout = m_workspace->GetPath() + "/./stdout";
    std::string task_stderr = m_workspace->GetPath() + "/./stderr";
    *stdout_fd = open(task_stdout.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
                      S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    *stderr_fd = open(task_stderr.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
                      S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

void AbstractTaskRunner::StartTaskAfterFork(std::vector<int>& fd_vector,int stdout_fd,int stderr_fd){
    // do in child process,
    // all interface called in child process should be async-safe.
    // NOTE if dup2 will return errno == EINTR?
    while (dup2(stdout_fd, STDOUT_FILENO) == -1 && errno == EINTR) {}
    while (dup2(stderr_fd, STDERR_FILENO) == -1 && errno == EINTR) {}
    for (size_t i = 0; i < fd_vector.size(); i++) {
        if (fd_vector[i] == STDOUT_FILENO
                    || fd_vector[i] == STDERR_FILENO
                    || fd_vector[i] == STDIN_FILENO) {
                // do not deal with std input/output
            continue;
         }
         close(fd_vector[i]);
    }

    chdir(m_workspace->GetPath().c_str());
    passwd *pw = getpwnam(FLAGS_task_acct.c_str());
    if (NULL == pw) {
        assert(0);
    }
    uid_t userid = getuid();
    if (0 == userid) {
        //chroot(m_workspace->GetPath().c_str());
        if (pw->pw_uid != userid) {
            setuid(pw->pw_uid);
        }
    }

    char *argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                    const_cast<char*>(m_task_info.cmd_line().c_str()),NULL};
    std::stringstream task_id_env;
    task_id_env <<"TASK_ID="<<m_task_info.task_offset();
    std::stringstream task_num_env;
    task_num_env <<"TASK_NUM="<<m_task_info.job_replicate_num();
    std::stringstream task_user_env;
    task_user_env <<"USER="<<pw->pw_uid;
    char *env[] = {const_cast<char*>(task_id_env.str().c_str()),
                   const_cast<char*>(task_num_env.str().c_str()),
                   const_cast<char*>(task_user_env.str().c_str()),
                   NULL};

    execve("/bin/sh", argv, env);
    LOG(FATAL,"fail to kill exec %s errno %d %s",
        m_task_info.task_name().c_str(), errno, strerror(errno));
    assert(0);
    _exit(127);
}

void AbstractTaskRunner::StartMonitorAfterFork(std::vector<int>& fd_vector,int stdout_fd,int stderr_fd) {
    // do in child process,
    // all interface called in child process should be async-safe.
    // NOTE if dup2 will return errno == EINTR?
    while (dup2(stdout_fd, STDOUT_FILENO) == -1 && errno == EINTR) {}
    while (dup2(stderr_fd, STDERR_FILENO) == -1 && errno == EINTR) {}
    for (size_t i = 0; i < fd_vector.size(); i++) {
        if (fd_vector[i] == STDOUT_FILENO
                || fd_vector[i] == STDERR_FILENO
                || fd_vector[i] == STDIN_FILENO) {
            // do not deal with std input/output
                continue;
        }
        close(fd_vector[i]);
    }
    chdir(m_workspace->GetPath().c_str());
    char cur_path[1024] = {0};
    getcwd(cur_path, 1024);
    std::string conf_path = std::string(cur_path) + "/galaxy_monitor/";
    std::string conf_file = conf_path + "monitor.conf";
    chdir(conf_path.c_str());
    std::string cmd_line = std::string("/home/galaxy/monitor/")
        + std::string("monitor_agent --monitor_conf=") + conf_file;
    char *argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
        const_cast<char*>(cmd_line.c_str()), NULL};
    std::stringstream task_id_env;
    task_id_env <<"TASK_ID="<<m_task_info.task_offset();
    char *env[] = {const_cast<char*>(task_id_env.str().c_str()), NULL};
    execve("/bin/sh", argv, env);
    LOG(FATAL,"fail to  exec monitor %s errno %d %s",
            cmd_line.c_str(), errno, strerror(errno));
    assert(0);
    _exit(127);
}
int AbstractTaskRunner::ReStart(){
    // only has retry times reach limit return -1
    int max_retry_times = FLAGS_task_retry_times;
    if (m_task_info.has_fail_retry_times()) {
        max_retry_times = m_task_info.fail_retry_times();
    }
    if (m_has_retry_times
            >= max_retry_times) {
        SetStatus(ERROR);
        return -1;
    }

    m_has_retry_times ++;
    if (Stop() != 0) {
        // stop failed need retry last heartbeat
        return 0;
    }

    Start();
    StartMonitor();
    return 0;
}

void CommandTaskRunner::StopPost() {
    if (collector_ != NULL) {
        collector_->Clear();
    }
    std::string meta_file = persistence_path_dir_ 
        + "/" + RUNNER_META_PREFIX 
        + boost::lexical_cast<std::string>(sequence_id_);
    if (!file::Remove(meta_file)) {
        LOG(WARNING, "rm meta failed rm %s", 
                meta_file.c_str());
    }
}

CommandTaskRunner::~CommandTaskRunner() {
    ResourceCollectorEngine* engine
        = GetResourceCollectorEngine();
    engine->DelCollector(collector_id_);
    if (collector_ != NULL) {
        delete collector_;
        collector_ = NULL;
    }
}

void CommandTaskRunner::Status(TaskStatus* status) {
    if (collector_ != NULL) {
        status->set_cpu_usage(collector_->GetCpuUsage());
        status->set_memory_usage(collector_->GetMemoryUsage());
        LOG(WARNING, "cpu usage %f memory usage %ld",
                status->cpu_usage(), status->memory_usage());
    }
    
    status->set_job_id(m_task_info.job_id());
    LOG(INFO, "task with id %ld state %d", 
            m_task_info.task_id(),
            m_task_state);
    // check if it is running
    int ret = IsRunning();
    if (ret == 0) {
        SetStatus(RUNNING);
        status->set_status(RUNNING);
    }
    else if (ret == 1) {
        SetStatus(COMPLETE);
        status->set_status(COMPLETE);
    }
    // last state is running ==> download finish
    else if (m_task_state == RUNNING
            || m_task_state == RESTART) {
        SetStatus(RESTART);
        if (ReStart() == 0) {
            status->set_status(RESTART);
        }
        else {
            SetStatus(ERROR);
            status->set_status(ERROR);
        }
    }
    // other state
    else {
        LOG(DEBUG, "task with id %ld state %d",
                m_task_info.task_id(),
                m_task_state);
        status->set_status(m_task_state);
    }
    return;
}

//start process
//1. fork a subprocess A
//2. exec command in the subprocess A
//TODO add workspace
int CommandTaskRunner::Start() {
    LOG(INFO, "start a task with id %d", m_task_info.task_id());
    if (IsRunning() == 0) {
        LOG(WARNING, "task with id %d has been runing", m_task_info.task_id());
        SetStatus(RUNNING);
        return -1;
    }
    int stdout_fd,stderr_fd;
    std::vector<int> fds;
    PrepareStart(fds,&stdout_fd,&stderr_fd);
    //sequence_id_ ++;
    passwd *pw = getpwnam(FLAGS_task_acct.c_str());
    if (NULL == pw) {
        LOG(WARNING, "getpwnam %s failed", FLAGS_task_acct.c_str());
        return -1;
    }
    uid_t userid = getuid();
    if (pw->pw_uid != userid && 0 == userid) {
        if (!file::Chown(m_workspace->GetPath(), pw->pw_uid, pw->pw_gid)) {
            LOG(WARNING, "chown %s failed", m_workspace->GetPath().c_str());
            return -1;
        }
    }

    m_child_pid = fork();
    //child
    if (m_child_pid == 0) {
        pid_t my_pid = getpid();
        int ret = setpgid(my_pid, my_pid);
        if (ret != 0) {
            assert(0);
        }
        std::string meta_file = persistence_path_dir_
            + "/" + RUNNER_META_PREFIX
            + boost::lexical_cast<std::string>(sequence_id_);
        int meta_fd = open(meta_file.c_str(), O_WRONLY | O_CREAT, S_IRWXU);
        if (meta_fd == -1) {
            assert(0);
        }
        int64_t value = my_pid;
        int len = write(meta_fd, (void*)&value, sizeof(value));
        if (len == -1) {
            close(meta_fd);
            assert(0);
        }
        if (0 != fsync(meta_fd)) {
            close(meta_fd);
            assert(0);
        }
        close(meta_fd);
        StartTaskAfterFork(fds,stdout_fd,stderr_fd);
    } else {
        close(stdout_fd);
        close(stderr_fd);
        if (m_child_pid == -1) {
            LOG(WARNING, "task with id %ld fork failed err[%d: %s]",
                    m_task_info.task_id(),
                    errno,
                    strerror(errno));
            SetStatus(ERROR);
            return -1;
        }
        SetStatus(RUNNING);
        m_group_pid = m_child_pid;
        // NOTE not multi thread safe
        if (collector_ == NULL) {
            collector_ = new ProcResourceCollector(m_child_pid);
            ResourceCollectorEngine* engine
                = GetResourceCollectorEngine();
            collector_id_ = engine->AddCollector(collector_);
        }
        else {
            collector_->ResetPid(m_child_pid);
        }
    }
    return 0;
}

int CommandTaskRunner::StartMonitor() 
{
    LOG(INFO, "start a task with id %ld", m_task_info.task_id());
    if (0 != m_task_info.monitor_conf().size()) {
        return -1;
    }
    if (IsProcessRunning(m_monitor_pid) == 0) {
        LOG(WARNING, "task with id %d has been monitoring", m_task_info.task_id());
        return -1;
    }
    int stdout_fd, stderr_fd;
    std::vector<int> fds;
    PrepareStart(fds, &stdout_fd, &stderr_fd);

    std::string::size_type replace_start =
        m_task_info.monitor_conf().find("<intput>");
    replace_start =
        m_task_info.monitor_conf().find(":");
    std::string::size_type replace_end =
        m_task_info.monitor_conf().find("\n", replace_start);

    char cur_path[1024] = {0};
    if (NULL == getcwd(cur_path, 1024)) {
        LOG(WARNING, "get cur path err [%d:%s]", errno, strerror(errno));
    }
    std::string log_path = std::string(cur_path) + "/"
        + m_workspace->GetPath().substr(1, m_workspace->GetPath().size() - 1)
        + m_task_info.monitor_conf().substr(replace_start + 1,
                replace_end - replace_start - 1);

    std::string new_conf = m_task_info.monitor_conf();

    new_conf.replace(replace_start + 1, replace_end - replace_start - 1, log_path);
    std::string monitor_conf = m_workspace->GetPath() + "/galaxy_monitor/monitor.conf";
    int conf_fd = open(monitor_conf.c_str(), O_WRONLY | O_CREAT, S_IRWXU);
    if (conf_fd == -1) {
        LOG(WARNING, "open monitor_conf %s failed [%d:%s]", 
                monitor_conf.c_str(),errno, strerror(errno));
    } else {
        int len = write(conf_fd, (void*)new_conf.c_str(),
                new_conf.size());
        if (len == -1) {
            LOG(WARNING, "write monitor_conf %s failed [%d:%s]",monitor_conf.c_str(),
                    errno, strerror(errno));
        }
        close(conf_fd);
    }
    m_monitor_pid = fork();
    if (m_monitor_pid == 0) {
        pid_t my_pid = getpid();
        int ret = setpgid(my_pid, my_pid);
        if (ret != 0) {
            assert(0);
        }
        StartMonitorAfterFork(fds, stdout_fd, stderr_fd);
    } else {
        close(stdout_fd);
        close(stderr_fd);
        if (m_monitor_pid == -1) {
            LOG(WARNING, "monitor with id %ld fork failed err[%d: %s]",
                    m_task_info.task_id(),
                    errno,
                    strerror(errno));
            return -1;
        }
        m_monitor_gid = m_monitor_pid;
    }
    return 0;
}

int CommandTaskRunner::Prepare() {
    int ret = Start();
    if (ret != 0) {
        return ret;
    }
    StartMonitor();
    return ret;
}

bool CommandTaskRunner::RecoverRunner(const std::string& persistence_path) {
    std::vector<std::string> files;
    if (!file::GetDirFilesByPrefix(
                persistence_path,
                RUNNER_META_PREFIX,
                &files)) {
        LOG(WARNING, "get meta files failed");
        return false;
    }
    LOG(DEBUG, "get meta files size %lu", files.size());
    if (files.size() == 0) {
        return true;
    }
    int max_seq_id = -1;
    std::string last_meta_file;
    for (size_t i = 0; i < files.size(); i++) {
        std::string file = files[i];
        size_t pos = file.find(RUNNER_META_PREFIX);
        if (pos == std::string::npos) {
            continue;
        }

        if (pos + RUNNER_META_PREFIX.size() >= file.size()) {
            LOG(WARNING, "meta file format err %s", file.c_str());
            continue;
        }

        int cur_id = atoi(file.substr(pos + RUNNER_META_PREFIX.size()).c_str());
        if (max_seq_id < cur_id) {
            max_seq_id = cur_id;
            last_meta_file = file;
        }
    }
    if (max_seq_id < 0) {
        return false;
    }

    std::string meta_file = last_meta_file;
    LOG(DEBUG, "start to recover %s", meta_file.c_str());
    int fin = open(meta_file.c_str(), O_RDONLY);
    if (fin == -1) {
        LOG(WARNING, "open meta file failed %s err[%d: %s]",
                meta_file.c_str(),
                errno,
                strerror(errno));
        return false;
    }

    size_t value;
    int len = read(fin, (void*)&value, sizeof(value));
    if (len == -1) {
        LOG(WARNING, "read meta file failed err[%d: %s]",
                errno,
                strerror(errno));
        close(fin);
        return false;
    }
    close(fin);

    LOG(DEBUG, "recove gpid %lu", value);
    int ret = killpg((pid_t)value, SIGKILL);
    if (ret != 0 && errno != ESRCH) {
        LOG(WARNING, "fail to kill process group %lu", value);
        return false;
    }
    return true;
}

}
