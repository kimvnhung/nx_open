// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "manager_linux.h"

#include <fcntl.h>
#include <linux/joystick.h>

#include <QtCore/QDir>

#include <nx/utils/log/log_main.h>

#include "device_linux.h"
#include "descriptors.h"

namespace nx::vms::client::desktop::joystick {

ManagerLinux::ManagerLinux(QObject* parent):
    base_type(parent)
{
}

void ManagerLinux::enumerateDevices()
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    QSet<QString> foundDevices;

    QDir inputsDir("/dev/input/");
    inputsDir.setFilter(QDir::AllEntries | QDir::System);
    QFileInfoList joystickInputFiles = inputsDir.entryInfoList({"js*"});
    for (const auto& joystickInput: joystickInputFiles)
    {
        const QString path = joystickInput.absoluteFilePath();
        foundDevices << path;
        if (m_devices.contains(path))
            continue;

        int fd = open(joystickInput.absoluteFilePath().toLatin1().data(), O_RDONLY | O_NONBLOCK);
        if (fd == -1)
            continue;

        static constexpr int kNameMaxSize = 256;
        char name[kNameMaxSize];
        __u32 version;
        __u8 axesNumber;
        __u8 buttonsNumber;

        ioctl(fd, JSIOCGNAME(kNameMaxSize), name);
        ioctl(fd, JSIOCGVERSION, &version);
        ioctl(fd, JSIOCGAXES, &axesNumber);
        ioctl(fd, JSIOCGBUTTONS, &buttonsNumber);

        close(fd);

        const QString modelName(name);
        NX_VERBOSE(this,
            "A new Joystick has been found. "
            "Model: %1, path: %2",
            modelName, path);

        const auto config = getDeviceDescription(modelName);
        DeviceLinux* deviceLinux = new DeviceLinux(config, path, pollTimer());
        DevicePtr device(deviceLinux);
        deviceLinux->setFoundControlsNumber(axesNumber, buttonsNumber);

        if (device->isValid())
            initializeDevice(device, config, path);
        else
            NX_VERBOSE(this, "Device is invalid. Model: %1, path: %2", modelName, path);
    }

    removeUnpluggedJoysticks(foundDevices);
}

} // namespace nx::vms::client::desktop::joystick
