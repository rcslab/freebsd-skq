# $NetBSD: deptgt.mk,v 1.10 2020/12/27 18:20:26 rillig Exp $
#
# Tests for special targets like .BEGIN or .SUFFIXES in dependency
# declarations.

# TODO: Implementation

# Just in case anyone tries to compile several special targets in a single
# dependency line: That doesn't work, and make immediately rejects it.
.SUFFIXES .PHONY: .c.o

# The following lines demonstrate how 'targets' is set and reset during
# parsing of dependencies.  To see it in action, set breakpoints in:
#
#	ParseDoDependency	at the beginning
#	FinishDependencyGroup	at "targets = NULL"
#	Parse_File		at "Lst_Free(targets)"
#	Parse_File		at "targets = Lst_New()"
#	ParseLine_ShellCommand	at "targets == NULL"
#
# Keywords:
#	parse.c:targets

target1 target2: sources	# targets := [target1, target2]
	: command1		# targets == [target1, target2]
	: command2		# targets == [target1, target2]
VAR=value			# targets := NULL
	: command3		# parse error, since targets == NULL

# In a dependency declaration, the list of targets can be empty.
# It doesn't matter whether the empty string is generated by a variable
# expression or whether it is just omitted.
.MAKEFLAGS: -dp
${:U}: empty-source
	: command for empty targets list
: empty-source
	: command for empty targets list
.MAKEFLAGS: -d0

# Just to show that a malformed expression is only expanded once in
# ParseDependencyTargetWord.  The only way to produce an expression that
# is well-formed on the first expansion and ill-formed on the second
# expansion would be to use the variable modifier '::=' to modify the
# targets.  This in turn would be such an extreme and unreliable edge case
# that nobody uses it.
$$$$$$$${:U:Z}:

all:
	@:;
