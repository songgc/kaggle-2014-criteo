#!/usr/bin/env python3

import argparse, csv, sys, math

from common import *

if len(sys.argv) == 1:
    sys.argv.append('-h')

parser = argparse.ArgumentParser(description='convert to svm')
parser.add_argument('-n', default=10000000, type=int, help='set number of bins for hashing trick')
parser.add_argument('csv_path', type=str, help='set path to the csv file')
parser.add_argument('svm_path', type=str, help='set path to the svm file')
args = vars(parser.parse_args())

max_values = {1: 5775.0, 2: 257675.0, 3: 65535.0, 4: 969.0, 5: 23159456.0, 6: 431037.0, 7: 56311.0, 8: 6047.0, 9: 29019.0, 10: 11.0, 11: 231.0, 12: 4008.0, 13: 7393.0}

with open(args['svm_path'], 'w') as f:
    for i, row in enumerate(csv.reader(open(args['csv_path']))):
        if i == 0:
            continue
        feats = set()
        label = row[1]
        for i, element in enumerate(row[2:15], start=1):
            bin = hashstr(str(i)+str(element), args['n'])+28
            feats.add((bin, 1))
            if element == '':
                continue
            value = float(element)
            feats.add((i, value/max_values[i]))
            if value < 1:
                continue
            value = math.log(float(element))
            feats.add((i+14, value))
        for i, element in enumerate(row[15:], start=1):
            bin = hashstr(str(i)+element, args['n'])+28
            feats.add((bin, 1))
        feats = list(feats)
        feats.sort()
        feats = ['{0}:{1}'.format(idx, val) for (idx, val) in feats]
        f.write(label + ' ' + ' '.join(feats) + '\n')
