## HOW TO CONTRIBUTE TO HYPERFLUX DEVELOPMENT

Hyperflux is a fork of Axel. Hyperflux is available at
<https://example.invalid/hyperflux>.

If you are interested in contributing to Hyperflux development, please follow
these steps:

1. Send a patch that fixes an issue or that implements a new feature.
   Alternatively, you can do a 'pull request'[1] in GitHub.

[1]: https://help.github.com/articles/using-pull-requests

2. Ask to join the Hyperflux project, if you want to work officially. Note
   that this second step is not compulsory. However, to accept you in the
   project, a minimum of previous collaboration is needed.


To find issues and bugs to fix, check the project page at
<https://example.invalid/hyperflux>.

If you want to join, please make a contact.

  -- Eriberto, Sun, 20 Mar 2016 16:27:53 -0300,
     updated on Sun, 08 Sep 2017 23:27:00 -0300.

## Submitting Changes

### Coding style
As of version 2.15, Hyperflux adopted a new coding style, very similar to that of the
Linux Kernel, with the additional requirement to insert a newline after the
return type of procedure declarations.

To aid the transition for imported code, an `.indent.pro` file is provided in
the top level source directory.  It should work with both GNU and BSD
implementations of `indent`, although the results may be slightly different.

Small variations are acceptable, and *non-compliance* in existing code, by
itself, *is not something to fix*.

### Conditions
By making a contribution to the project you certify that either you hold
copyright to the work; or you have obtained permission, or are implicitly
permitted by the work's licensing terms (to the best of your knowledge), to
submit it under a license compatible with the licensing terms of the project
(See the Licensing section in this document).

You also understand that this project is public, and agree your contribution and
all personal information submitted with it will be kept indefinitely and may be
redistributed with the project.

In order to keep track of the source of a contribution, each party involved in
the sumbission must sign-off the commit, meaning that they abide by the
aforementioned rules.

### Licensing
Hyperflux is provided under the terms of the GNU General Public License version 2 or
(at your option) any later version, as described in the COPYING file, plus an
exception for linking against OpenSSL 1.x.

By submitting code for inclusion in the project you agree to license it under
these terms, or more permissive ones.

Contributions made with a different licensing must state it explicitly in the
file header.

Here's the wording in the header of each file licensed under GPL-2.0:

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	In addition, as a special exception, the copyright holders give
	permission to link the code of portions of this program with the
	OpenSSL library under certain conditions as described in each
	individual source file, and distribute linked combinations including
	the two.

	You must obey the GNU General Public License in all respects for all
	of the code used other than OpenSSL. If you modify file(s) with this
	exception, you may extend this exception to your version of the
	file(s), but you are not obligated to do so. If you do not wish to do
	so, delete this exception statement from your version. If you delete
	this exception statement from all source files in the program, then
	also delete it here.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
