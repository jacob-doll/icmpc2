#!/bin/bash
host="http://pwnboard.win/generic"
[ "$1" = "" ] && exit
[ "$2" != "" ] && name="$2" || name="bash"
data="{\"ip\":\"$1\",\"type\":\"$name\"}"
header='Content-Type: application/json'
curl -X POST -H "$header" -H "$h2" $host -d "$data" &> /dev/null