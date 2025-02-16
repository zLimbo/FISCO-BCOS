#!/bin/bash

set -e

# SHELL_FOLDER=$(cd $(dirname $0);pwd)
current_dir=$(pwd)
key_path=""
gmkey_path=""
output_dir="newNode"
logfile="build.log"
conf_path="conf"
gm_conf_path="gmconf/"
TASSL_CMD="${HOME}"/.fisco/tassl
guomi_mode=
sdk_cert=
gen_rsa_cert="false"
gen_rsa_sdk_cert="true"
days=36500
rsa_key_length=2048

LOG_WARN()
{
    local content=${1}
    echo -e "\033[31m[WARN] ${content}\033[0m"
}

LOG_INFO()
{
    local content=${1}
    echo -e "\033[32m[INFO] ${content}\033[0m"
}

help() {
    cat << EOF
Usage:
    -c <cert path>              [Required] cert key path
    -g <gm cert path>           gmcert key path, if generate gm node cert
    -s                          If set -s, generate certificate for sdk
    -R                          If set -R, generate certificate for sdk with ecdsa, default rsa, work with -s
    -o <Output Dir>             Default ${output_dir}
    -h Help
e.g
    $0 -c nodes/cert/agency -o newNode                                #generate node certificate
    $0 -c nodes/cert/agency -o newSDK -s                              #generate sdk certificate
    $0 -c nodes/cert/agency -g nodes/gmcert/agency -o newNode_GM      #generate gm node certificate
    $0 -c nodes/cert/agency -g nodes/gmcert/agency -o newSDK_GM -s    #generate gm sdk certificate
EOF

exit 0
}

check_and_install_tassl(){
    if [ ! -f "${TASSL_CMD}" ];then
        LOG_INFO "Downloading tassl binary ..."
        if [[ "$(uname)" == "Darwin" ]];then
            curl --insecure -#LO https://github.com/FISCO-BCOS/LargeFiles/raw/master/tools/tassl_mac.tar.gz
            mv tassl_mac.tar.gz tassl.tar.gz
        else
            curl --insecure -#LO https://github.com/FISCO-BCOS/LargeFiles/raw/master/tools/tassl.tar.gz
        fi
        tar zxvf tassl.tar.gz && rm tassl.tar.gz
        chmod u+x tassl
        mkdir -p "${HOME}"/.fisco
        mv tassl "${HOME}"/.fisco/tassl
    fi
}

parse_params()
{
while getopts "c:o:g:hRsX:" option;do
    case $option in
    c) [ ! -z $OPTARG ] && key_path=$OPTARG
    ;;
    o) [ ! -z $OPTARG ] && output_dir=$OPTARG
    ;;
    g) guomi_mode="yes" && gmkey_path=$OPTARG
        check_and_install_tassl
    ;;
    R) gen_rsa_sdk_cert="false";;
    s) sdk_cert="true";;
    X) days="$OPTARG";;
    h) help;;
    esac
done
}

print_result()
{
echo "=============================================================="
LOG_INFO "Cert Path   : $key_path"
[ ! -z "${guomi_mode}" ] && LOG_INFO "GM Cert Path: $gmkey_path"
LOG_INFO "Output Dir  : $output_dir"
LOG_INFO "RSA cert    : ${gen_rsa_cert}"
LOG_INFO "RSA SDK cert    : ${gen_rsa_sdk_cert}"
echo "=============================================================="
LOG_INFO "All completed. Files in $output_dir"
}

getname() {
    local name="$1"
    if [ -z "$name" ]; then
        return 0
    fi
    [[ "$name" =~ ^.*/$ ]] && {
        name="${name%/*}"
    }
    name="${name##*/}"
    echo "$name"
}

check_name() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[a-zA-Z0-9._-]+$ ]] || {
        echo "$name name [$value] invalid, it should match regex: ^[a-zA-Z0-9._-]+\$"
        exit 1
    }
}

exit_with_clean()
{
    local content=${1}
    echo -e "\033[31m[ERROR] ${content}\033[0m"
    if [ -d "${output_dir}" ];then
        rm -rf "${output_dir}"
    fi
    exit 1
}

file_must_exists() {
    if [ ! -f "$1" ]; then
        exit_with_clean "$1 file does not exist, please check!"
    fi
}

file_must_not_exists() {
    if [ -f "$1" ]; then
        LOG_WARN "$1 file exists, please check!"
        exit 1
    fi
}

dir_must_exists() {
    if [ ! -d "$1" ]; then
        exit_with_clean "$1 DIR does not exist, please check!"
    fi
}

dir_must_not_exists() {
    if [ -e "$1" ]; then
        LOG_WARN "$1 DIR exists, please clean old DIR!"
        exit 1
    fi
}

gen_cert_secp256k1() {
    capath="$1"
    certpath="$2"
    name="$3"
    type="$4"
    openssl ecparam -out $certpath/${type}.param -name secp256k1
    openssl genpkey -paramfile "$certpath/${type}.param" -out "$certpath/${type}.key" 2> /dev/null
    openssl pkey -in "$certpath/${type}.key" -pubout -out "$certpath/${type}.pubkey" 2> /dev/null
    openssl req -new -sha256 -subj "/CN=${name}/O=fisco-bcos/OU=${type}" -key "$certpath/${type}.key" -out "$certpath/${type}.csr"
    openssl x509 -req -days ${days} -sha256 -in "$certpath/${type}.csr" -CAkey "$capath/agency.key" -CA "$capath/agency.crt" \
        -force_pubkey "$certpath/${type}.pubkey" -out "$certpath/${type}.crt" -CAcreateserial -extensions v3_req -extfile "$capath/cert.cnf" 2> /dev/null
    openssl ec -in "$certpath/${type}.key" -outform DER 2> /dev/null | tail -c +8 | head -c 32 | xxd -p -c 32 | cat >"$certpath/${type}.private"
    rm -f $certpath/${type}.csr
}

gen_node_cert() {
    if [ "" == "$(openssl ecparam -list_curves 2>&1 | grep secp256k1)" ]; then
        echo "openssl don't support secp256k1, please upgrade openssl!"
        exit -1
    fi
    agpath="${1}"
    agency=$(basename "$agpath")
    ndpath="${2}"
    node="node"
    dir_must_exists "$agpath"
    file_must_exists "$agpath/agency.key"
    check_name agency "$agency"
    dir_must_not_exists "$ndpath"
    check_name node "$node"

    mkdir -p $ndpath
    gen_cert_secp256k1 "$agpath" "$ndpath" "$node" "node"
    #nodeid is pubkey
    openssl ec -text -noout -in "$ndpath/node.key" 2> /dev/null| sed -n '7,11p' | tr -d ": \n" | awk '{print substr($0,3);}' | cat >"$ndpath/node.nodeid"
    cp $agpath/ca.crt $agpath/agency.crt $ndpath
}

gen_node_cert_with_extensions_gm() {
    capath="$1"
    certpath="$2"
    name="$3"
    type="$4"
    extensions="$5"

    $TASSL_CMD genpkey -paramfile $capath/gmsm2.param -out $certpath/gm${type}.key
    $TASSL_CMD req -new -subj "/CN=$name/O=fiscobcos/OU=agency" -config "$capath/gmcert.cnf" -key "$certpath/gm${type}.key" -out "$certpath/gm${type}.csr"
    $TASSL_CMD x509 -sm3 -req -CA $capath/gmagency.crt -CAkey $capath/gmagency.key -days ${days} -CAcreateserial -in $certpath/gm${type}.csr -out $certpath/gm${type}.crt -extfile $capath/gmcert.cnf -extensions $extensions

    rm -f $certpath/gm${type}.csr
}

gen_node_cert_gm() {

    agpath="${1}"
    agency=$(basename "$agpath")
    ndpath="${2}"
    node=$(basename "$ndpath")
    dir_must_exists "$agpath"
    file_must_exists "$agpath/gmagency.key"
    check_name agency "$agency"

    mkdir -p $ndpath
    dir_must_exists "$ndpath"
    check_name node "$node"

    mkdir -p $ndpath
    gen_node_cert_with_extensions_gm "$agpath" "$ndpath" "$node" node v3_req
    gen_node_cert_with_extensions_gm "$agpath" "$ndpath" "$node" ennode v3enc_req
    #nodeid is pubkey
    $TASSL_CMD ec -in $ndpath/gmnode.key -text -noout | sed -n '7,11p' | sed 's/://g' | tr "\n" " " | sed 's/ //g' | awk '{print substr($0,3);}'  | cat > $ndpath/gmnode.nodeid

    cp $agpath/gmca.crt $agpath/gmagency.crt $ndpath
    cd $ndpath
}

gen_rsa_chain_cert() {
    local name="${1}"
    local chaindir="${2}"

    mkdir -p "${chaindir}"

    LOG_INFO "chaindir: ${chaindir}"

    file_must_not_exists "${chaindir}"/ca.key
    file_must_not_exists "${chaindir}"/ca.crt
    # file_must_exists "${cert_conf}"

    mkdir -p "$chaindir"
    dir_must_exists "$chaindir"

    openssl genrsa -out "${chaindir}"/ca.key "${rsa_key_length}" 2>/dev/null
    openssl req -new -x509 -days "${days}" -subj "/CN=${name}/O=fisco-bcos/OU=chain" -key "${chaindir}"/ca.key -out "${chaindir}"/ca.crt  2>/dev/null

    LOG_INFO "Generate rsa ca cert successfully!"
}

gen_rsa_node_cert() {
    local capath="${1}"
    local ndpath="${2}"
    local type="${3}"

    file_must_exists "$capath/ca.key"
    file_must_exists "$capath/ca.crt"
    # check_name node "$node"

    file_must_not_exists "$ndpath"/"${type}".key
    file_must_not_exists "$ndpath"/"${type}".crt

    mkdir -p "${ndpath}"
    dir_must_exists "${ndpath}"

    openssl genrsa -out "${ndpath}"/"${type}".key "${rsa_key_length}" 2>/dev/null
    openssl req -new -sha256 -subj "/CN=FISCO-BCOS/O=fisco-bcos/OU=agency" -key "$ndpath"/"${type}".key -out "$ndpath"/"${type}".csr
    openssl x509 -req -days "${days}" -sha256 -CA "${capath}"/ca.crt -CAkey "$capath"/ca.key -CAcreateserial \
        -in "$ndpath"/"${type}".csr -out "$ndpath"/"${type}".crt -extensions v4_req 2>/dev/null

    openssl pkcs8 -topk8 -in "$ndpath"/"${type}".key -out "$ndpath"/pkcs8_node.key -nocrypt

    rm -f "$ndpath"/"$type".csr
    rm -f "$ndpath"/"$type".key

    mv "$ndpath"/pkcs8_node.key "$ndpath"/"$type".key
    cp "$capath/ca.crt" "$ndpath"

    # LOG_INFO "Generate ${ndpath} node rsa cert successful!"
}

generate_script_template()
{
    local filepath=$1
    cat << EOF > "${filepath}"
#!/bin/bash
SHELL_FOLDER=\$(cd \$(dirname \$0);pwd)

EOF
    chmod +x ${filepath}
}

generate_node_scripts()
{
    local output=$1
    generate_script_template "$output/start.sh"
    cat << EOF >> "$output/start.sh"
fisco_bcos=\${SHELL_FOLDER}/../fisco-bcos
cd \${SHELL_FOLDER}
node=\$(basename \${SHELL_FOLDER})
node_pid=\`ps aux|grep "\${fisco_bcos}"|grep -v grep|awk '{print \$2}'\`
if [ ! -z \${node_pid} ];then
    echo " \${node} is running, pid is \$node_pid."
    exit 0
else
    nohup \${fisco_bcos} -c config.ini 2>>nohup.out &
    sleep 0.5
fi
node_pid=\`ps aux|grep "\${fisco_bcos}"|grep -v grep|awk '{print \$2}'\`
if [ ! -z \${node_pid} ];then
    echo " \${node} start successfully"
else
    echo " \${node} start failed"
    cat nohup.out
fi
EOF
    generate_script_template "$output/stop.sh"
    cat << EOF >> "$output/stop.sh"
fisco_bcos=\${SHELL_FOLDER}/../fisco-bcos
node=\$(basename \${SHELL_FOLDER})
node_pid=\`ps aux|grep "\${fisco_bcos}"|grep -v grep|awk '{print \$2}'\`
try_times=5
i=0
while [ \$i -lt \${try_times} ]
do
    if [ -z \${node_pid} ];then
        echo " \${node} isn't running."
        exit 0
    fi
    [ ! -z \${node_pid} ] && kill \${node_pid}
    sleep 0.4
    node_pid=\`ps aux|grep "\${fisco_bcos}"|grep -v grep|awk '{print \$2}'\`
    if [ -z \${node_pid} ];then
        echo " stop \${node} success."
        exit 0
    fi
    ((i=i+1))
done
EOF
}

main(){
    if [ ! -z "$(openssl version | grep reSSL)" ];then
        export PATH="/usr/local/opt/openssl/bin:$PATH"
    fi

    # gen rsa ca for channel
    if [ -f "${key_path}/channel/ca.key" ] && [ -f "${key_path}/channel/ca.crt" ]; then
        gen_rsa_cert="true"
    fi

    while :
    do
        gen_node_cert "${key_path}" "${output_dir}"
        cd ${output_dir}
        mkdir -p ${conf_path}/
        mv *.* ${conf_path}/
        cd ${current_dir}
        #private key should not start with 00
        privateKey=$(openssl ec -in "${output_dir}/${conf_path}/node.key" -text 2> /dev/null| sed -n '3,5p' | sed 's/://g'| tr "\n" " "|sed 's/ //g')
        len=${#privateKey}
        head2=${privateKey:0:2}
        if [ "64" != "${len}" ] || [ "00" == "$head2" ];then
            rm -rf ${output_dir}
            continue;
        fi
        if [ -n "$guomi_mode" ]; then
            gen_node_cert_gm ${gmkey_path} ${output_dir} > ${logfile} 2>&1
            mkdir -p ${gm_conf_path}/
            mv ./*.* ${gm_conf_path}/
            cd ${current_dir}
            #private key should not start with 00
            privateKey=$($TASSL_CMD ec -in "${output_dir}/${gm_conf_path}/gmnode.key" -text 2> /dev/null| sed -n '3,5p' | sed 's/://g'| tr "\n" " "|sed 's/ //g')
            len=${#privateKey}
            head2=${privateKey:0:2}
            if [ "64" != "${len}" ] || [ "00" == "$head2" ];then
                rm -rf ${output_dir}
                continue;
            fi
        fi
        break;
    done

    # gen rsa cert for node
    if [ "${gen_rsa_cert}" == "true" ];then 
        gen_rsa_node_cert "${key_path}/channel" "${output_dir}/${conf_path}/channel_cert" "node"
    fi

    # generate_node_scripts "${output_dir}"
    cat ${key_path}/agency.crt >> ${output_dir}/${conf_path}/node.crt
    cat ${key_path}/ca.crt >> ${output_dir}/${conf_path}/node.crt
    if [ -n "$guomi_mode" ]; then
        cat ${gmkey_path}/gmagency.crt >> ${output_dir}/${gm_conf_path}/gmnode.crt
        cat ${gmkey_path}/gmca.crt >> ${output_dir}/${gm_conf_path}/gmnode.crt

        #move origin conf to gm conf
        # rm ${output_dir}/${conf_path}/node.nodeid
        if [ -d "${output_dir}/${conf_path}/channel_cert" ]; then
            mv "${output_dir}/${conf_path}/channel_cert" ${output_dir}/${gm_conf_path}
        fi

        cp -r ${output_dir}/${conf_path} ${output_dir}/${gm_conf_path}/origin_cert
        #remove original cert files
        rm -rf ${output_dir:?}/${conf_path}
        mv ${output_dir}/${gm_conf_path} ${output_dir}/${conf_path}
    fi
    if [[ -n "${sdk_cert}" ]]; then
        if [ -n "$guomi_mode" ]; then
            mv "${output_dir}/${conf_path}/gmnode.key" "${output_dir}/${conf_path}/gmsdk.key"
            mv "${output_dir}/${conf_path}/gmnode.crt" "${output_dir}/${conf_path}/gmsdk.crt"
            mv "${output_dir}/${conf_path}/gmennode.key" "${output_dir}/${conf_path}/gmensdk.key"
            mv "${output_dir}/${conf_path}/gmennode.crt" "${output_dir}/${conf_path}/gmensdk.crt"
            mv "${output_dir}/${conf_path}/gmnode.nodeid" "${output_dir}/${conf_path}/gmsdk.publickey"

            if [ "${gen_rsa_sdk_cert}" == "true" ];then
                mv "${output_dir}/${conf_path}/channel_cert/node.key" "${output_dir}/sdk.key"
                mv "${output_dir}/${conf_path}/channel_cert/node.crt" "${output_dir}/sdk.crt"
                mv "${output_dir}/${conf_path}/channel_cert/ca.crt" "${output_dir}/ca.crt"
            else
                mv "${output_dir}/${conf_path}/origin_cert/node.key" "${output_dir}/sdk.key"
                mv "${output_dir}/${conf_path}/origin_cert/node.crt" "${output_dir}/sdk.crt"
                mv "${output_dir}/${conf_path}/origin_cert/ca.crt" "${output_dir}/ca.crt"
                mv "${output_dir}/${conf_path}/origin_cert/node.nodeid" "${output_dir}/sdk.publickey"
            fi

            rm -rf "${output_dir:?}/${conf_path}/origin_cert"
            mv "${output_dir}/${conf_path}" "${output_dir}/gm"
        else
            if [ "${gen_rsa_sdk_cert}" == "true" ];then 
                mv "${output_dir}/${conf_path}/channel_cert/node.key" "${output_dir}/sdk.key"
                mv "${output_dir}/${conf_path}/channel_cert/node.crt" "${output_dir}/sdk.crt"
                mv "${output_dir}/${conf_path}/channel_cert/ca.crt" "${output_dir}/ca.crt"
            else 
                mv "${output_dir}/${conf_path}/node.key" "${output_dir}/sdk.key"
                mv "${output_dir}/${conf_path}/node.nodeid" "${output_dir}/sdk.publickey"
                mv "${output_dir}/${conf_path}/node.crt" "${output_dir}/sdk.crt"
                mv "${output_dir}/${conf_path}/ca.crt" "${output_dir}/ca.crt"
            fi
          
            rm -rf "${output_dir:?}/${conf_path}"
        fi
    fi

    if [ -f "${logfile}" ];then rm "${logfile}";fi

}

parse_params $@
main
print_result
