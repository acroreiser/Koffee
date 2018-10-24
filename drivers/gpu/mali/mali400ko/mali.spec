%define kernel_target           u8500
%define kernel_version  	%(find /lib/modules -name "*%{kernel_target}" | cut -c 14-)

Name: mali400ko
License: GPL
Summary: Mali400 kernel module
Version: 1.0
Release: 1
URL: http://stericsson.com
BuildRoot: %{_tmppath}/%{name}
Requires: kernel
BuildRequires: kernel-u8500-devel
Requires(post): ldconfig
Requires(postun): ldconfig

Source0: mali400ko.tar.gz

%description
Mali400 kernel module.

%files
%defattr(-,root,root)
/lib/modules/%{kernel_version}/extra/mali.ko
/lib/modules/%{kernel_version}/extra/mali_drm.ko

%prep
%setup -q

%build
export CROSS_COMPILE=""
export KERNELDIR=/usr/src/kernels/%{kernel_version}
export KERNEL_BUILD_DIR=/usr/src/kernels/%{kernel_version}
export USING_UMP=1
export USING_HWMEM=1
#make V=0 mali-devicedrv

%install
export CROSS_COMPILE=""
export KERNELDIR=/usr/src/kernels/%{kernel_version}
export KERNEL_BUILD_DIR=/usr/src/kernels/%{kernel_version}
export USING_UMP=1
export USING_HWMEM=1
export INSTALL_MOD_DIR=extra
export INSTALL_MOD_PATH=$RPM_BUILD_ROOT
make V=0 install-mali install-mali_drm

# Remove kernel modules.* files
rm -f $RPM_BUILD_ROOT/lib/modules/%{kernel_version}/modules.*

%clean
rm -rf $RPM_BUILD_ROOT

%post

%postun


