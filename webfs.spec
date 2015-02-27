Name:       webfs
Summary:    lightweight http server for static content
Version:    1.21
Release:    0
Source0:    %{name}-%{version}.tar.gz
Copyright:  GPL
Group:      Network/Daemons
Buildroot:  %{_tmppath}/root-%{name}-%{version}

%description
This is a simple http server for purely static content.  You
can use it to serve the content of a ftp server via http for
example.  It is also nice to export some files the quick way
by starting a http server in a few seconds, without editing
some config file first.

%prep
%setup -q

%build
export CFLAGS="$RPM_OPT_FLAGS"
make prefix=/usr

%install
if test "%{buildroot}" != ""; then
	rm -rf "%{buildroot}"
fi
make prefix=/usr DESTDIR=%{buildroot} install

%files
%defattr(-,root,root)
/usr/bin/webfsd
/usr/share/man/man1/webfsd.1*
%doc README COPYING

%clean
if test "%{buildroot}" != ""; then
	rm -rf "%{buildroot}"
fi
