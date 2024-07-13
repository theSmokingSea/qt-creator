import shutil
import os
import subprocess

def copy_dll(ori_path, tar_path):
    shutil.copy(ori_path, tar_path)

def remove_dll(path):
    os.remove(path)

def run_deployqt(path: str):
    win_deploy_qt = "C:/software/DevTools/Qt/Qt6.6/6.6.1/msvc2019_64/bin/windeployqt.exe"

    arguments = [path]

    result = subprocess.run([win_deploy_qt] + arguments, capture_output=True, text=True)
    # 获取标准输出和标准错误
    output = result.stdout
    error_output = result.stderr
    # 打印输出
    print(path)
    print("Process output:", output)
    print("Process error output:", error_output)


if __name__ == "__main__":
    build_path = "C:/Code/Personal/qt-creator/cmake-build-"
    build_type = ["release", "debug"]

    for build in build_type:
        path = build_path + build
        bin_path =  path + "/bin/"
        succ = ["Utils.dll","qtcreator.exe","ExtensionSystem.dll"]
        succ_path = path + "/bin/"

        plugins_path = path + "/lib/qtcreator/plugins/"
        extral_dll = ["Core.dll", "Designer.dll", "ImageViewer.dll", "Help.dll"]
        # 处理qtcreator
        run_deployqt(succ_path+succ[0])
        run_deployqt(succ_path+succ[1])
        run_deployqt(succ_path+succ[2])

        for extra in extral_dll:
            # 拷贝
            copy_dll(plugins_path+extra, bin_path)

            run_deployqt(bin_path+extra)

            remove_dll(bin_path+extra)

  


