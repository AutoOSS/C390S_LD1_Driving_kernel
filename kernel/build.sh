#!/bin/bash

# When unexpected situations occur during script execution, exit immediately to avoid errors being ignored and incorrect final results
set -e

# start export core-utils functions for everyone project
. core-utils.sh
# end

function selecte_config
{
    if [ "$HR_BASE_BOARD_TYPE" = "qemu" ]
	then
		export KERNEL_DEFCONFIG="hobot_j6x_${HR_BASE_BOARD_TYPE}_defconfig"
	else
		export KERNEL_DEFCONFIG="hobot_${HR_TARGET_PROJECT}_defconfig"
	fi
}

function make_defconfig
{
	echo "kernel defconfig: ${KERNEL_DEFCONFIG}"

	make ARCH=${ARCH_KERNEL} O=${OUT_DIR} $KERNEL_DEFCONFIG || {
		echo "make $KERNEL_DEFCONFIG failed"
		exit 1
	}

	if [ "${HR_TARGET_MODE}" == "debug" ]; then
		make ARCH=${ARCH_KERNEL} O=${OUT_DIR} hobot_${HR_TARGET_PROJECT}_debug.config || {
			echo "make hobot_${HR_TARGET_PROJECT}_debug.config failed"
			exit 1
		}
	fi

	if [ "${ENABLE_KERNEL_DEBUG}" == "true" ]; then
		make ARCH=${ARCH_KERNEL} O=${OUT_DIR} hobot_j6x_debug.config || {
			echo "make hobot_j6x_debug.config failed"
			exit 1
		}
		make ARCH=${ARCH_KERNEL} O=${OUT_DIR} hobot_j6x_debug_more.config || {
			echo "make hobot_j6x_debug_more.config failed"
			exit 1
		}
	fi

	if [ "${HR_TARGET_MODE}" == "release" ]; then
		# make ARCH=${ARCH_KERNEL} O=${OUT_DIR} hobot_j6x_sec.config || {
		# 	echo "make hobot_j6x_sec.config failed"
		# 	exit 1
		# }
		make ARCH=${ARCH_KERNEL} O=${OUT_DIR} hobot_j6x_module_sig.config || {
			echo "make hobot_j6x_module_sig.config failed"
			exit 1
		}
		mkdir -p ${OUT_DIR}/certs
		cp ${KERNEL_MODULE_SIG_KEYS_NAME}.* ${OUT_DIR}/certs/
	fi
}

function all
{
	local deb_out_dir=${PRODUCT_DEB_DIR}/runtime-pkg

	make_defconfig

	# Specify target Image.lz4, ko is not compiled
	# make ARCH=${ARCH_KERNEL} O=${OUT_DIR} -j${N} Image.lz4 || {
	# 	echo "make failed"
	# 	exit 1
	# }

	rm -rf ${OUT_DIR}/../linux-* ${deb_out_dir}/linux-*
	# Ko is compiled here
	make ARCH=${ARCH_KERNEL} O=${OUT_DIR} -j${N} bindeb-pkg || {
		echo "make $KERNEL_DEFCONFIG failed"
		exit 1
	}

	make O=${OUT_DIR} compile_commands.json || {
		echo "make compile_commands.json failed"
		exit 1
	}

	# if [ -f ${OUT_DIR}/compile_commands.json ] && [ ! -f ${SRC_KERNEL_DIR}/compile_commands.json ] ; then
	# 	ln -s ${OUT_DIR}/compile_commands.json ${SRC_KERNEL_DIR}/compile_commands.json
	# fi

	# if [ -f $SRC_HOBOT_TOOLS_DIR/bear/.clangd_aarch64 ] && [ ! -f ${SRC_KERNEL_DIR}/.clangd ]; then
	# 	ln -s $SRC_HOBOT_TOOLS_DIR/bear/.clangd_aarch64 ${SRC_KERNEL_DIR}/.clangd
	# fi

	if [ -f ${OUT_DIR}/include/config/kernel.release ]; then
		mkdir -p ${TARGET_DEPLOY_DIR}/${ROOTFS_NAME}/
		cp -v ${OUT_DIR}/include/config/kernel.release ${TARGET_DEPLOY_DIR}/${ROOTFS_NAME}/.kernelrelease
	fi
	mkdir -p ${deb_out_dir}/
	cp -avRf ${OUT_DIR}/../linux-image-*.deb ${deb_out_dir}/
}

function clean
{
	echo "build output path: ${OUT_DIR}"
	make O=${OUT_DIR} mrproper
}

function compile_mod
{
	local dir=$1
	local mod=$2
	if [[ "$3" == "clean" ]];then
		local op="clean"
	else
		local op="modules"
	fi

	if [ ! -f ${OUT_DIR}/Module.symvers ]; then
		echo "Please fully compile the kernel first"
		exit
	fi

	make_defconfig

	if [[ x"$dir" == x"kernel-driver" ]]; then
		make O=${OUT_DIR} M=drivers/$mod -j${N} $op
	elif [[ x"$dir" == x"hobot-driver" ]]; then
		rel_parh=$(realpath --relative-to=$(pwd) ${HR_TOP_DIR}/hobot-drivers)
		make O=${OUT_DIR} M=$rel_parh/$mod -j${N} $op
	else
		echo "Specify the directory where the driver is located, kernel-driver or hobot-driver"
		exit
	fi
}

function build_kernel_targets
{
	make_defconfig
	make O=${OUT_DIR} -j${N} "$@"
}

cd ${SRC_KERNEL_DIR}

# export OUT_DIR
set_out_path

selecte_config

cmd=$1

if [ "$cmd" = "clean" ]; then
    echo "start to clean"
    clean
elif [ "$cmd" = "all" ]; then
    echo "start to compile kernel:"
    all
elif [ "$cmd" = "bkt" ]; then
    echo "start to compile kernel targets:"
    build_kernel_targets "${@:2}"
else
	echo "start to compile $2 module"
	compile_mod "$@"
fi
