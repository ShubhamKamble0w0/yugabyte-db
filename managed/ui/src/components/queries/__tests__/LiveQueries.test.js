import { filterBySearchTokens } from '../queriesHelper';

jest.mock('../../../pages/Login');

const mockQueries = [
  {
    nodeName: 'test-node-driver-1',
    privateIp: '192.180.10.1',
    keyspace: 'potatomachine',
    query: 'SELECT * FROM cars WHERE age < 10',
    type: 'active',
    elapsedMillis: 3,
    clientHost: '10.19.120.7',
    clientPort: 9042
  },
  {
    nodeName: 'test-node-republic-2',
    privateIp: '192.180.10.2',
    keyspace: 'potatomachine',
    query: 'INSERT INTO recipe VALUES (101, "Easiest Potato Oven Recipes", 5, 8)',
    type: 'active',
    elapsedMillis: 15,
    clientHost: '10.19.120.7',
    clientPort: 9042
  },
  {
    nodeName: 'test-node-driver-2',
    privateIp: '192.180.10.3',
    keyspace: 'potatomachine',
    query: 'SELECT * FROM cars WHERE brand = "Ford"',
    type: 'active',
    elapsedMillis: 10,
    clientHost: '10.19.120.7',
    clientPort: 9042
  },
];
const mockSearchTokens = [
  {
    key: 'nodeName',
    label: 'Node Name',
    value: 'driver'
  }
];

it('Search Bar: single filter on column and value', () => {
  expect(filterBySearchTokens(mockQueries, mockSearchTokens).length).toBe(2);
});

it('Search Bar: multiple filters on column and value', () => {
  mockSearchTokens.push({
    key: 'elapsedMillis',
    label: 'Elapsed Time',
    value: '<5'
  });
  expect(filterBySearchTokens(mockQueries, mockSearchTokens).length).toBe(1);
});

it('Search Bar: double wildcard range should not work', () => {
  mockSearchTokens.push({
    key: 'elapsedMillis',
    label: 'Elapsed Time',
    value: '*..*'
  });
  expect(filterBySearchTokens(mockQueries, mockSearchTokens).length).toBe(0);
});